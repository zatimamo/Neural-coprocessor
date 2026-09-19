"""MGPU-AutoLab - running the standalone PROCESSCONTEXT diagnostic.

WHAT IT RUNS, AND WHY IN THIS SHAPE
    Four arms, each an independent process, in the fixed order the experiment
    defines:

        single          nothing else at all - THE REFERENCE
        dual-held       second device created and held
        dual-active     second device plus a DIRECT queue and a live heap
        dual-released   second device created, recorded, then released

    Each arm writes its own `context-<mode>.log`. The four are then parsed
    separately and combined into one result.

    This repeats exactly what tools/processcontext_ab/RUN-CONTEXT-AB.cmd does -
    including running the executable under a name carrying the `nvngx.dll`
    prefix, which the DLSSNR snippet's caller check requires - but under Python
    control, so each arm gets its own timeout and its own captured output. A
    single timeout covering all four arms cannot say which arm stalled.

THE REFERENCE GATE
    SINGLE is the control. If it does not reproduce the working result - both
    private calls NVAPI_OK, blob 3944768, Reserved18 Success, a non-zero handle -
    then nothing else in this diagnostic is readable, and the graph stops with
    PROCESSCONTEXT_REFERENCE_INVALID. The gate is declared as data in
    experiments.PROCESSCONTEXT_REFERENCE_GATE and evaluated here; it is not
    restated as prose.

THE ARTIFACT'S OWN LAUNCHER IS WHAT RUNS
    The node does not re-implement the four-arm sequence. It stages the built
    artifact and launches RUN-CONTEXT-AB.cmd - the same script a human would run
    - once for the SINGLE gate phase and once for the remaining three. That is
    what makes the run reproducible by hand: there is one launcher, and AutoLab
    calls it rather than a copy of it.

    The launcher is given an explicit arm list for each phase, which is why it
    can stop after SINGLE. With no arm list it still runs all four and prints
    the fixed table, exactly as before.

THE REFERENCE RUNTIME AND THE DATA PATH
    --nr-dll is the NEURALSCREEN REFERENCE runtime, located from configuration
    and hash-verified. It is passed through UNTOUCHED: the diagnostic derives the
    NGX data path as dirname(--nr-dll), so copying the DLL into AutoLab's own
    staging directory would move the reference data path away from
    NeuralScreen's. The staging directory holds only the artifact's own files.

THE SPLIT-PROCESS NODE, AND WHY IT HAS ITS OWN PHASE 1
    SPLITPROCESS_ISOLATION answers one question: does the RTX 4070 lane still
    fail when the active Ti SUPER D3D12 state is alive in a DIFFERENT process?

        PHASE 1   the SAME processcontext_ab.exe --mode single, alone, through the
                  same launcher. The current environment must reproduce the
                  eight-condition reference IMMEDIATELY BEFORE the holder test.
                  Historical success is not a substitute: if this fails the run
                  stops as SPLITPROCESS_REFERENCE_INVALID and holder_ti.exe is
                  never launched.
        PHASE 2   holder_ti.exe (PROCESS A) is launched and must report
                  HOLDER_READY while remaining alive; then the SAME
                  processcontext_ab.exe runs the SAME single arm as PROCESS B;
                  then the holder is stopped and must report HOLDER_STOPPED.

    THE EXECUTABLE IS HASHED BEFORE EACH LAUNCH AND THE TWO HASHES MUST MATCH.
    If the binary changed between PHASE 1 and PHASE 2 the two phases are not the
    same experiment, PROCESS B is not launched and the run is not readable.

    PROCESS A IS A SEPARATE PROGRAM. It is holder_ti.exe from the same artifact,
    not a mode of the diagnostic: the diagnostic's sources are frozen at the
    implementation that produced the PROCESSCONTEXT evidence, and the CI gate
    proves it. Nothing about PROCESS B is forked, re-implemented or
    parameterised differently from the PROCESSCONTEXT reference arm.

NOTHING IS DEPLOYED
    The diagnostic runs from a staging directory under the run's results tree.
    The game's add-on is untouched by this node, and Cyberpunk is never launched.
"""

import ctypes
import os
import shutil
import subprocess
import threading
import time

import experiments
import log_parser
from github_actions import sha256_file


class ProcessContextError(Exception):
    pass


#: PROCESS A of the split-process node. A SEPARATE EXECUTABLE, shipped in the
#: same artifact, in its own process - never a mode of processcontext_ab.exe,
#: because the binary running the RTX 4070 lane has to be the proven one.
HOLDER_EXE_NAME = "holder_ti.exe"

#: The holder's protocol. It prints exactly these, alone on a line.
HOLDER_READY = "HOLDER_READY"
HOLDER_FAILED = "HOLDER_FAILED"
HOLDER_STOPPED = "HOLDER_STOPPED"

#: The holder writes its own log; AutoLab names the file so the archive has a
#: deterministic source.
HOLDER_LOG_NAME = "context-holder.log"

#: PHASE 1's log is preserved under this name before PHASE 2 overwrites
#: context-single.log - both phases run the same arm, so they share a file name.
PHASE1_LOG_NAME = "phase1-reference.log"


# ---------------------------------------------------------------------------
# The named stop event
# ---------------------------------------------------------------------------
# The holder waits on a NAMED EVENT that the parent owns. It is created here,
# before the holder starts, so there is exactly one owner and a stale event from
# an earlier run cannot be inherited by a later one.
#
# If the event cannot be created at all the holder still stops: closing its
# stdin is the second signal, and it is the one that matters if this process
# dies. That is why a failure here is logged and not raised.

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True) if os.name == "nt" else None
if _kernel32 is not None:
    _kernel32.CreateEventW.restype = ctypes.c_void_p
    _kernel32.CreateEventW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                       ctypes.c_wchar_p]
    _kernel32.OpenEventW.restype = ctypes.c_void_p
    _kernel32.OpenEventW.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_wchar_p]
    _kernel32.SetEvent.restype = ctypes.c_int
    _kernel32.SetEvent.argtypes = [ctypes.c_void_p]
    _kernel32.CloseHandle.restype = ctypes.c_int
    _kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

_EVENTS = {}


def holder_event_create(name):
    if _kernel32 is None:
        return None
    handle = _kernel32.CreateEventW(None, 1, 0, name)   # manual reset, initially clear
    if not handle:
        return None
    _EVENTS[name] = handle
    return handle


def holder_event_signal(name):
    if _kernel32 is None or not name:
        return False
    handle = _EVENTS.get(name)
    if not handle:
        handle = _kernel32.OpenEventW(0x0002, 0, name)   # EVENT_MODIFY_STATE
        if not handle:
            return False
        _EVENTS[name] = handle
    return bool(_kernel32.SetEvent(handle))


def holder_event_close(name):
    if _kernel32 is None or not name:
        return
    handle = _EVENTS.pop(name, None)
    if handle:
        _kernel32.CloseHandle(handle)


#: The artifact's own launcher. AutoLab calls THIS rather than re-implementing
#: the four-arm sequence, so what runs is the script a human would run.
LAUNCHER_NAME = "RUN-CONTEXT-AB.cmd"

#: The name the executable must run under. The snippet inspects its caller's
#: module file name and refuses anything without this substring. The launcher
#: performs this rename; recorded here only so the fact is visible in one place.
GATE_PREFIX = "nvngx.dll_"


class ProcessContextRunner(object):
    def __init__(self, cfg, log, gh=None):
        self.cfg = cfg
        self.log = log
        self.gh = gh
        self.timeout = int(cfg["timeouts"]["processcontext_seconds"])
        self.cache_dir = cfg["_cache_dir"]
        self.install_dir = cfg["paths"]["install_dir"]
        self.nr_dll_rel = cfg["paths"]["nr_dll"].replace("/", os.sep)
        # THE REFERENCE RUNTIME. Absolute, from configuration, verified by the
        # caller and again in stage().
        self.ns_nr_dll = (cfg["paths"].get("neuralscreen_nr_dll") or "").replace("/", os.sep)
        self.required_nr_sha = cfg["required_nr_dll_sha256"].lower()
        # How long to wait for the holder to establish its state, and how long to
        # wait for it to stop at each graded step.
        self.holder_ready_timeout = int(cfg["timeouts"].get("holder_ready_seconds", 60))
        self.holder_stop_timeout = int(cfg["timeouts"].get("holder_stop_seconds", 15))

    # -------------------------------------------------------------- staging
    def stage(self, artifact_meta, staging_dir):
        """Lay out the artifact in a run directory.

        The NR runtime is NOT copied. --nr-dll is handed the reference path
        itself, because the diagnostic derives the NGX data path from it and that
        directory has to be NeuralScreen's.
        """
        artifact_dir = artifact_meta["artifact_dir"]
        payload = os.path.join(artifact_dir, "_pcab")
        if not os.path.isdir(payload):
            payload = artifact_dir
        exe = os.path.join(payload, "processcontext_ab.exe")
        if not os.path.isfile(exe):
            raise ProcessContextError(
                "the processcontext artifact does not contain processcontext_ab.exe "
                "(looked in %s)" % payload)
        holder = os.path.join(payload, HOLDER_EXE_NAME)
        if not os.path.isfile(holder):
            raise ProcessContextError(
                "the processcontext artifact does not contain %s, so PROCESS A of the "
                "split-process node cannot be launched (looked in %s)"
                % (HOLDER_EXE_NAME, payload))
        launcher = os.path.join(payload, LAUNCHER_NAME)
        if not os.path.isfile(launcher):
            raise ProcessContextError(
                "the processcontext artifact does not contain %s, so the four arms "
                "cannot be driven the way a human would drive them" % LAUNCHER_NAME)

        # LOCATE AND VERIFY THE REFERENCE RUNTIME, here as well as in preflight:
        # this is the file the lane is measured against, and a stale copy beside
        # the exe would be picked up silently if it were staged instead.
        nr_dll = self.ns_nr_dll
        if not nr_dll or not os.path.isfile(nr_dll):
            raise ProcessContextError(
                "the NeuralScreen reference runtime is not at %r. Set "
                "paths.neuralscreen_nr_dll in config.yaml." % (nr_dll or "",))
        got = sha256_file(nr_dll)
        if got.lower() != self.required_nr_sha:
            raise ProcessContextError(
                "the NeuralScreen reference runtime does not match the required build:"
                "\n  path     : %s\n  actual   : %s\n  required : %s"
                % (nr_dll, got.upper(), self.required_nr_sha.upper()))
        self.log("experiment PROCESSCONTEXT: reference runtime %s" % nr_dll)
        self.log("experiment PROCESSCONTEXT: sha256 %s OK" % got.upper())
        self.log("experiment PROCESSCONTEXT: derived data path will be %s"
                 % os.path.dirname(nr_dll))

        os.makedirs(staging_dir, exist_ok=True)
        for name in ("processcontext_ab.exe", HOLDER_EXE_NAME, LAUNCHER_NAME, "README.txt"):
            src = os.path.join(payload, name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(staging_dir, name))

        return {"dir": staging_dir, "exe": os.path.join(staging_dir, "processcontext_ab.exe"),
                "holder_exe": os.path.join(staging_dir, HOLDER_EXE_NAME),
                "holder_sha256": (sha256_file(os.path.join(staging_dir, HOLDER_EXE_NAME)).lower()
                                  if os.path.isfile(os.path.join(staging_dir, HOLDER_EXE_NAME))
                                  else None),
                "launcher": os.path.join(staging_dir, LAUNCHER_NAME),
                "nr_dll": nr_dll, "data_path": os.path.dirname(nr_dll)}

    # ------------------------------------------------------------------ run
    def run_launcher(self, staged, arms):
        """LAUNCH RUN-CONTEXT-AB.cmd for exactly these arms.

        The artifact's own launcher, not a re-implementation of it: it performs
        the nvngx.dll_-prefixed copy that the snippet's caller gate requires, and
        it is the same script a human runs. An explicit arm list is what lets the
        SINGLE gate stop the other three before they are ever started.
        """
        started = time.time()
        comspec = os.environ.get("COMSPEC", "cmd.exe")
        cmd = [comspec, "/c", staged["launcher"], staged["nr_dll"]] + list(arms)
        self.log("experiment PROCESSCONTEXT: launching %s %s (arms: %s)"
                 % (LAUNCHER_NAME, staged["nr_dll"], " ".join(arms)))
        try:
            proc = subprocess.run(cmd, cwd=staged["dir"], capture_output=True,
                                  text=True, timeout=self.timeout, errors="replace")
            out, err, rc = proc.stdout or "", proc.stderr or "", proc.returncode
        except subprocess.TimeoutExpired as exc:
            out = (exc.stdout or b"").decode("utf-8", "replace") \
                if isinstance(exc.stdout, bytes) else (exc.stdout or "")
            err, rc = "TIMEOUT after %ss" % self.timeout, None
        except OSError as exc:
            out, err, rc = "", "could not run the launcher: %s" % exc, None
        elapsed = round(time.time() - started, 2)
        self.log("experiment PROCESSCONTEXT: launcher (%s) exit %s in %.1fs"
                 % (" ".join(arms), rc, elapsed))
        for line in out.splitlines():
            if line.strip():
                self.log("  launcher| %s" % line.rstrip())
        if err.strip():
            for line in err.splitlines():
                if line.strip():
                    self.log("  launcher! %s" % line.rstrip())
        return {"arms": list(arms), "returncode": rc, "stdout": out, "stderr": err,
                "seconds": elapsed}

    def run_arm(self, staged, arm):
        """Run ONE arm through the launcher. Kept for the argparse-less path."""
        return self.run_launcher(staged, [arm])

    def run_arms(self, staged):
        """All four through the launcher in one invocation (no gate)."""
        return {"all": self.run_launcher(staged, experiments.PROCESSCONTEXT_ARMS)}

    def run_table(self, staged):
        """The artifact's own table, captured as evidence. Not a decision input."""
        try:
            proc = subprocess.run([staged["exe"], "--mode", "table"],
                                  cwd=staged["dir"], capture_output=True, text=True,
                                  timeout=120, errors="replace")
            return {"returncode": proc.returncode, "stdout": proc.stdout or ""}
        except (OSError, subprocess.SubprocessError) as exc:
            return {"returncode": None, "stdout": "", "error": str(exc)}

    # ---------------------------------------------------------------- parse
    def parse_arms(self, staged, outputs):
        return parse_arm_logs(staged["dir"])

    # --------------------------------------------------------------- result
    def result_from_arms(self, arms, artifact_meta, outputs, table, seconds):
        return build_result(arms, artifact_meta, outputs, table, seconds)

    # ---------------------------------------------------------------- entry
    def run(self, artifact_meta, run_dir, reporter=None):
        """SINGLE first through the launcher, then - ONLY if it passes - the rest.

        THE REFERENCE IS A GATE, NOT A DATUM. It is the control, so if it does
        not reproduce, nothing the other three arms could show would be
        readable: a difference between arms cannot be attributed to the variable
        when the baseline itself is broken.

        Both phases go through RUN-CONTEXT-AB.cmd. The launcher is given an
        explicit arm list, which is the only reason SINGLE can stop the other
        three before they are ever started.
        """
        staging = os.path.join(run_dir, "processcontext")
        staged = self.stage(artifact_meta, staging)
        started = time.time()
        outputs = {}
        arms = {}
        reference_arm = experiments.PROCESSCONTEXT_REFERENCE_ARM

        # ---- 1. SINGLE, alone, through the launcher ----------------------
        outputs[reference_arm] = self.run_launcher(staged, [reference_arm])
        parse_arm_logs(staged["dir"], arms=[reference_arm], out=arms)
        single = arms.get(reference_arm, {})
        reference_ok = single.get("reference_ok") is True

        # NOTE: SINGLE runs exactly ONCE. The launcher is given an explicit arm
        # list, so the second phase never re-runs it and context-single.log is
        # never overwritten - which is why there is no separate "gate" copy of
        # the log to keep.

        if not reference_ok:
            failing = single.get("reference_gate_failing") or []
            detail = "; ".join(
                "%s %s%s (saw %r)" % (c.get("field"), c.get("op"),
                                      "" if "value" not in c else " %r" % c["value"],
                                      single.get(c.get("field")))
                for c in failing) or "the arm produced no result to evaluate"
            self.log("experiment PROCESSCONTEXT: SINGLE did NOT pass the reference gate")
            self.log("experiment PROCESSCONTEXT: %s" % detail)
            self.log("experiment PROCESSCONTEXT: STOP - dual-held, dual-active and "
                     "dual-released were NOT launched")
            result = build_result(arms, artifact_meta, outputs, None,
                                  round(time.time() - started, 2))
            result["reason"] = ("the SINGLE reference arm did not reproduce, so the other "
                                "three arms were not launched: %s" % detail)
            result["observations"]["stopped_after"] = reference_arm
            result["observations"]["arms_not_launched"] = [
                a for a in experiments.PROCESSCONTEXT_ARMS if a != reference_arm]
            result["observations"]["staging_dir"] = staging
            result["observations"]["steps"] = [
                "locate the NeuralScreen reference runtime",
                "verify its SHA256",
                "launch %s (single)" % LAUNCHER_NAME,
                "parse the PCAB-RESULT record for single",
                "enforce SINGLE as the reference gate -> FAILED, STOP",
                "archive the arm log that exists",
            ]
            return result

        self.log("experiment PROCESSCONTEXT: SINGLE passed the reference gate; launching "
                 "the other three arms through the same launcher")

        # ---- 2. the rest, through the same launcher ---------------------
        rest = [a for a in experiments.PROCESSCONTEXT_ARMS if a != reference_arm]
        outputs["rest"] = self.run_launcher(staged, rest)
        parse_arm_logs(staged["dir"], arms=rest, out=arms)

        table = self.run_table(staged)
        result = build_result(arms, artifact_meta, outputs, table,
                              round(time.time() - started, 2))
        result["observations"]["stopped_after"] = None
        result["observations"]["staging_dir"] = staging
        result["observations"]["table_stdout"] = (table or {}).get("stdout", "")
        result["observations"]["steps"] = [
            "locate the NeuralScreen reference runtime",
            "verify its SHA256",
            "launch %s (single)" % LAUNCHER_NAME,
            "parse the PCAB-RESULT record for single",
            "enforce SINGLE as the reference gate -> PASSED",
            "launch %s (%s)" % (LAUNCHER_NAME, " ".join(rest)),
            "parse the PCAB-RESULT records for the remaining three arms",
            "classify with the fixed A/B/C/D/E rules",
            "archive all four arm logs",
        ]
        return result

    # ------------------------------------------------------- split-process
    def start_holder(self, staged, event_name, on_line=None):
        """PROCESS A: launch holder_ti.exe, wait for HOLDER_READY, prove it is alive.

        holder_ti.exe is the SEPARATE executable from the artifact, not a mode of
        the diagnostic: it holds active Ti SUPER D3D12 state and does nothing
        else. It is started with stdin as a PIPE we own, so that closing that
        pipe is a stop signal it honours even if everything else fails - and so
        that a holder can never outlive this process.
        """
        exe = staged["holder_exe"]
        cmd = [exe, "--event", event_name,
               "--seconds", "0",                 # 0 = no cap; the parent is the control
               "--log", HOLDER_LOG_NAME]
        self.log("experiment SPLITPROCESS_ISOLATION: PROCESS A - launching %s"
                 % HOLDER_EXE_NAME)
        self.log("experiment SPLITPROCESS_ISOLATION:   %s" % " ".join(cmd))
        proc = subprocess.Popen(cmd, cwd=staged["dir"], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors="replace", bufsize=1)
        holder = {"process": proc, "exe": exe, "event_name": event_name,
                  "ready": False, "failed": False, "stopped": False, "lines": [],
                  "returncode": None, "stop_reason": None}

        # Drain stdout on a thread: the holder may outlive this read loop, and a
        # full pipe would block it.
        def _drain():
            try:
                for line in proc.stdout:
                    line = line.rstrip("\r\n")
                    if not line:
                        continue
                    holder["lines"].append(line)
                    if on_line:
                        on_line(line)
                    if line.strip() == HOLDER_READY:
                        holder["ready"] = True
                    if line.strip() == HOLDER_FAILED:
                        holder["failed"] = True
                    if line.strip() == HOLDER_STOPPED:
                        holder["stopped"] = True
            except Exception:                     # noqa: BLE001 - the pipe closed
                pass

        thread = threading.Thread(target=_drain, daemon=True)
        thread.start()
        holder["thread"] = thread

        deadline = time.time() + self.holder_ready_timeout
        while time.time() < deadline:
            if holder["ready"]:
                break
            if proc.poll() is not None:
                break
            time.sleep(0.1)

        alive = proc.poll() is None
        holder["alive_after_ready"] = alive
        if holder["ready"] and alive:
            self.log("experiment SPLITPROCESS_ISOLATION: HOLDER_READY seen, and the holder "
                     "process is ALIVE (pid %s)" % proc.pid)
            return holder
        code = proc.poll()
        self.log("experiment SPLITPROCESS_ISOLATION: the holder did NOT reach HOLDER_READY "
                 "(ready=%s, alive=%s, exit=%s)"
                 % (holder["ready"], alive, code))
        self.stop_holder(holder)
        return holder

    def stop_holder(self, holder):
        """Stop PROCESS A, graded: named event, then stdin close, then terminate."""
        proc = holder.get("process")
        if proc is None:
            return holder
        if proc.poll() is not None:
            holder["returncode"] = proc.returncode
            return holder

        # 1. THE NAMED EVENT. This is the clean stop and the documented first
        #    choice; the holder opened it for exactly this.
        signalled = holder_event_signal(holder.get("event_name"))
        self.log("experiment SPLITPROCESS_ISOLATION: stopping the holder - named event %s"
                 % ("signalled" if signalled else "could NOT be signalled"))

        deadline = time.time() + self.holder_stop_timeout
        while time.time() < deadline and proc.poll() is None:
            time.sleep(0.1)

        # 2. STDIN. Closing the pipe is what the holder treats as "the parent is
        #    gone", so this works even if the event failed.
        if proc.poll() is None and proc.stdin is not None:
            self.log("experiment SPLITPROCESS_ISOLATION: the holder is still alive; closing "
                     "its stdin")
            try:
                proc.stdin.close()
            except Exception:                     # noqa: BLE001
                pass
            deadline = time.time() + self.holder_stop_timeout
            while time.time() < deadline and proc.poll() is None:
                time.sleep(0.1)

        # 3. LAST RESORT. Never leave a process holding GPU state.
        if proc.poll() is None:
            self.log("experiment SPLITPROCESS_ISOLATION: the holder is STILL alive; "
                     "terminating it")
            proc.terminate()
            try:
                proc.wait(timeout=self.holder_stop_timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

        holder["returncode"] = proc.returncode
        holder["stop_requested"] = True
        self.log("experiment SPLITPROCESS_ISOLATION: holder exited %s (HOLDER_STOPPED "
                 "seen: %s)" % (proc.returncode, bool(holder.get("stopped"))))
        return holder

    def run_split(self, artifact_meta, run_dir, reporter=None):
        """PHASE 1 the current reference; PHASE 2 PROCESS A + the same SINGLE arm.

        PHASE 1 exists because historical success is not evidence about TODAY. The
        exact executable that will run as PROCESS B must reproduce the
        eight-condition reference in this environment immediately beforehand; if
        it does not, the holder is never launched and the node stops as
        SPLITPROCESS_REFERENCE_INVALID, which is a statement about the machine and
        not about process isolation.

        PROCESS B is the SAME executable, the SAME launcher invocation and the
        SAME single arm as the PROCESSCONTEXT node uses. Its SHA256 is recorded
        before both launches and the two must be equal.
        """
        staging = os.path.join(run_dir, "splitprocess")
        staged = self.stage(artifact_meta, staging)
        started = time.time()
        reference_arm = experiments.PROCESSCONTEXT_REFERENCE_ARM
        event_name = "Local\\MGPU_PCAB_HOLDER_STOP_%lu" % os.getpid()

        outputs = {}
        arms = {}
        table = None
        holder = None
        process_b_ran = False
        bytes_changed = False

        # THE SAME BYTES IN BOTH PHASES. Taken before PHASE 1 and again before
        # PHASE 2: if the binary changed in between, the two phases are not the
        # same experiment and PROCESS B is not launched at all.
        sha_phase1 = sha256_file(staged["exe"]).lower()
        self.log("experiment SPLITPROCESS_ISOLATION: processcontext_ab.exe sha256 %s"
                 % sha_phase1.upper())
        if reporter is not None:
            reporter.hash("splitprocess.processcontext_ab.exe.phase1", sha_phase1,
                          path=staged["exe"])
            if staged.get("holder_sha256"):
                reporter.hash("splitprocess.%s" % HOLDER_EXE_NAME,
                              staged["holder_sha256"], path=staged["holder_exe"])

        # ---- PHASE 1: the current reference control ------------------------
        self.log("experiment SPLITPROCESS_ISOLATION: PHASE 1 - the reference control, "
                 "alone, in its own process")
        outputs["phase1"] = self.run_launcher(staged, [reference_arm])
        parse_arm_logs(staged["dir"], arms=[reference_arm], out=arms)
        phase1_summary = dict(arms.get(reference_arm, {}))
        phase1_ok = phase1_summary.get("reference_ok") is True
        phase1_detail = self._gate_detail(phase1_summary)
        self._keep_phase1_log(staged, reference_arm)

        if not phase1_ok:
            self.log("experiment SPLITPROCESS_ISOLATION: PHASE 1 did NOT pass the "
                     "reference gate - the executable that would run as PROCESS B does "
                     "not reproduce the reference in this environment right now")
            self.log("experiment SPLITPROCESS_ISOLATION: %s" % phase1_detail)
            self.log("experiment SPLITPROCESS_ISOLATION: STOP - %s was NOT launched"
                     % HOLDER_EXE_NAME)
            # `arms.single` means PROCESS B and nothing else. PHASE 1's numbers live
            # in observations.phase1, so a run in which PROCESS B never started can
            # never be read as if it had - PHASE 1's own log is in the staging
            # directory under the same file name.
            arms = {reference_arm: self._process_b_absent(
                reference_arm, "PHASE_1_REFERENCE_INVALID")}
            result = self._split_result(arms, phase1_summary, artifact_meta, outputs,
                                        table, started, staged, event_name, holder,
                                        phase1_ok=False, phase1_detail=phase1_detail,
                                        sha_phase1=sha_phase1, sha_phase2=None,
                                        process_b_ran=False, bytes_changed=False)
            return result

        self.log("experiment SPLITPROCESS_ISOLATION: PHASE 1 passed the reference gate; "
                 "the same executable will now run as PROCESS B")

        # ---- PHASE 2: PROCESS A holds the state, PROCESS B is the same arm --
        # The event is created and owned HERE, before the holder starts, so there
        # is exactly one owner and a stale event from an earlier run cannot be
        # inherited.
        holder_event_create(event_name)
        sha_phase2 = sha256_file(staged["exe"]).lower()
        if reporter is not None:
            reporter.hash("splitprocess.processcontext_ab.exe.phase2", sha_phase2,
                          path=staged["exe"])
        if sha_phase2 != sha_phase1:
            bytes_changed = True
            self.log("experiment SPLITPROCESS_ISOLATION: processcontext_ab.exe CHANGED "
                     "between PHASE 1 (%s) and PHASE 2 (%s)"
                     % (sha_phase1.upper(), sha_phase2.upper()))
        try:
            # ---- 1 + 2 + 3. PROCESS A, and prove it is alive --------------
            holder = self.start_holder(staged, event_name)

            if holder["ready"] and holder.get("alive_after_ready") and not bytes_changed:
                # ---- 4. PROCESS B: the same SINGLE arm, unchanged --------
                self.log("experiment SPLITPROCESS_ISOLATION: PROCESS B - the SAME SINGLE "
                         "arm, the same executable, the same %s invocation, in its own "
                         "process" % LAUNCHER_NAME)
                outputs[reference_arm] = self.run_launcher(staged, [reference_arm])
                process_b_ran = True
            elif bytes_changed:
                self.log("experiment SPLITPROCESS_ISOLATION: PROCESS B is NOT launched - "
                         "the executable changed between PHASE 1 and PHASE 2, so the two "
                         "phases would not be the same experiment")
            else:
                self.log("experiment SPLITPROCESS_ISOLATION: the holder did not establish "
                         "its state, so PROCESS B is NOT launched - there is nothing to "
                         "isolate and a run without the variable would answer nothing")
        finally:
            # ---- 7. terminate the holder cleanly, on EVERY path -----------
            if holder is not None:
                self.stop_holder(holder)
            holder_event_close(event_name)

        # ---- 5. parse PROCESS B's existing PCAB-RESULT -------------------
        if process_b_ran:
            arms = {}
            parse_arm_logs(staged["dir"], arms=[reference_arm], out=arms)
        else:
            holder_ok = bool(holder and holder["ready"]
                             and holder.get("alive_after_ready"))
            if bytes_changed:
                why = "EXECUTABLE_CHANGED_BETWEEN_PHASES"
            elif not holder_ok:
                why = "HOLDER_NOT_ESTABLISHED"
            else:
                why = "PROCESS_B_NOT_LAUNCHED"
            arms = {reference_arm: self._process_b_absent(reference_arm, why)}

        result = self._split_result(arms, phase1_summary, artifact_meta, outputs, table,
                                    started, staged, event_name, holder,
                                    phase1_ok=True, phase1_detail=phase1_detail,
                                    sha_phase1=sha_phase1, sha_phase2=sha_phase2,
                                    process_b_ran=process_b_ran,
                                    bytes_changed=bytes_changed)
        return result

    # ------------------------------------------------------------------ helpers
    @staticmethod
    def _process_b_absent(reference_arm, why):
        """The record for a PROCESS B that never ran.

        Deliberately NOT a parse of the staging directory: PHASE 1 wrote
        context-single.log there under the very name PROCESS B would have used, so
        reading the directory would report PHASE 1's numbers as PROCESS B's. An
        absent arm is reported as absent, with a reason, and every gate condition
        then fails to match - which is what routes the run to a STOP.
        """
        return {
            "mode": reference_arm, "valid": None, "reference_ok": None,
            "reference_gate_failing": [], "reason": why,
            "log_path": None, "log_present": False,
            "note": "PROCESS B did not run, so it has no result - it is NOT PHASE 1's",
        }

    @staticmethod
    def _gate_detail(summary):
        """The failing conditions, in words, for the log and the result reason."""
        failing = summary.get("reference_gate_failing") or []
        if not failing:
            return "the arm produced no result to evaluate"
        return "; ".join(
            "%s %s%s (saw %r)" % (c.get("field"), c.get("op"),
                                  "" if "value" not in c else " %r" % c["value"],
                                  summary.get(c.get("field")))
            for c in failing)

    def _keep_phase1_log(self, staged, reference_arm):
        """Preserve PHASE 1's arm log.

        Both phases run the SAME arm, so both write context-single.log and the
        second would overwrite the first. PHASE 1's log is the evidence that the
        reference reproduced immediately before the holder test, so it is kept
        under its own name before PHASE 2 starts.
        """
        src = os.path.join(staged["dir"], "context-%s.log" % reference_arm)
        if not os.path.isfile(src):
            return None
        dest = os.path.join(staged["dir"], PHASE1_LOG_NAME)
        try:
            shutil.copy2(src, dest)
        except OSError as exc:                        # noqa: BLE001
            self.log("experiment SPLITPROCESS_ISOLATION: could not preserve the PHASE 1 "
                     "log (%s)" % exc)
            return None
        return dest

    def _split_result(self, arms, phase1_summary, artifact_meta, outputs, table,
                      started, staged, event_name, holder, phase1_ok, phase1_detail,
                      sha_phase1, sha_phase2, process_b_ran, bytes_changed):
        """The split-process result object, in the common shape.

        `valid` here means ONE thing: this run produced a readable isolation
        outcome - the reference reproduced, the holder held, the holder stopped
        cleanly and PROCESS B actually ran. It is not the four-arm validity of
        the PROCESSCONTEXT node, which is why it is stated rather than derived
        from `arms`: only one arm is ever run here.
        """
        result = build_result(arms, artifact_meta, outputs, table,
                              round(time.time() - started, 2))
        result["kind"] = "splitprocess"
        result["experiment"] = "SPLITPROCESS_ISOLATION"
        single = dict(arms.get(experiments.PROCESSCONTEXT_REFERENCE_ARM, {}))

        holder_established = bool(holder and holder["ready"]
                                  and holder.get("alive_after_ready"))
        holder_stopped = bool(holder and holder.get("stopped"))
        process_b_ok = single.get("reference_ok") is True

        result["phase1_ok"] = bool(phase1_ok)
        result["holder_established"] = holder_established
        result["holder_stopped"] = holder_stopped
        result["process_b_ran"] = bool(process_b_ran)
        result["exe_same_bytes"] = not bytes_changed

        if not phase1_ok:
            result["valid"] = False
            result["reason"] = (
                "PHASE 1 REFERENCE INVALID: the executable that would run as PROCESS B "
                "did not reproduce the eight-condition reference in this environment "
                "immediately beforehand, so %s was not launched and this run says "
                "nothing about process isolation: %s" % (HOLDER_EXE_NAME, phase1_detail))
        elif not holder_established:
            result["valid"] = False
            result["reason"] = (
                "%s did not establish active Ti SUPER D3D12 state (ready=%s, alive=%s, "
                "exit=%s), so PROCESS B was not launched"
                % (HOLDER_EXE_NAME, bool(holder and holder["ready"]),
                   bool(holder and holder.get("alive_after_ready")),
                   holder["returncode"] if holder else None))
        elif not holder_stopped:
            result["valid"] = False
            result["reason"] = (
                "the holder held its state but did not report HOLDER_STOPPED (exit=%s), so "
                "the run did not end cleanly" % (holder["returncode"] if holder else None))
        elif bytes_changed:
            result["valid"] = False
            result["reason"] = (
                "the executable changed between PHASE 1 (%s) and PHASE 2 (%s), so the two "
                "phases are not the same experiment and PROCESS B was not launched"
                % (sha_phase1.upper(), sha_phase2.upper()))
        else:
            result["valid"] = True
            result["reason"] = (
                "PHASE 1 reproduced the reference with processcontext_ab.exe, %s held its "
                "state while the SAME executable ran the SAME single arm as PROCESS B, and "
                "the holder stopped cleanly" % HOLDER_EXE_NAME)

        result["observations"]["phase1"] = {
            "passed": bool(phase1_ok),
            "detail": phase1_detail,
            "summary": phase1_summary,
            "log": PHASE1_LOG_NAME,
            "returncode": (outputs.get("phase1") or {}).get("returncode"),
        }
        result["observations"]["holder"] = {
            "exe": holder["exe"] if holder else os.path.join(staged["dir"], HOLDER_EXE_NAME),
            "sha256": staged.get("holder_sha256"),
            "event_name": event_name,
            "ready": bool(holder and holder["ready"]),
            "failed_line": bool(holder and holder["failed"]),
            "alive_after_ready": bool(holder and holder.get("alive_after_ready")),
            "stopped_line": holder_stopped,
            "returncode": holder["returncode"] if holder else None,
            "log": HOLDER_LOG_NAME,
            "lines": list(holder["lines"]) if holder else [],
        }
        result["observations"]["executable_sha256"] = {
            "phase1": sha_phase1,
            "phase2": sha_phase2,
            # None, not True: in a run that never reached PHASE 2 there are no two
            # hashes to compare, and "identical" would be a claim about a launch
            # that did not happen.
            "identical": (None if sha_phase2 is None else not bytes_changed),
        }
        result["observations"]["process_b"] = (
            "the SAME SINGLE arm, the same executable and the same %s invocation the "
            "PROCESSCONTEXT node uses, in its own process" % LAUNCHER_NAME)
        result["observations"]["process_b_summary"] = single
        result["observations"]["staging_dir"] = staged["dir"]
        result["observations"]["steps"] = [
            "PHASE 1: launch %s (single) alone and hash the executable" % LAUNCHER_NAME,
            ("PHASE 1: reference gate -> PASSED" if phase1_ok
             else "PHASE 1: reference gate -> FAILED, STOP, holder NOT launched"),
            "PHASE 2: hash the executable again and compare with PHASE 1",
            "PHASE 2: launch %s as PROCESS A" % HOLDER_EXE_NAME,
            "wait for exactly HOLDER_READY",
            "verify the holder process is still alive",
            ("launch the SAME single arm as PROCESS B" if process_b_ran
             else "PROCESS B NOT launched"),
            "parse PROCESS B's existing PCAB-RESULT",
            "signal the named stop event and require HOLDER_STOPPED",
            "archive the PHASE 1 log, the holder log and PROCESS B's log",
        ]
        return result

    # -------------------------------------------------------------- archiving
    def archive_split_logs(self, result, reporter):
        """Archive the three logs the split-process node produces.

            holder.log                    PROCESS A, written by holder_ti.exe
            processcontext-single.log     PROCESS B, the same name the
                                          PROCESSCONTEXT node uses for the SINGLE
                                          arm, so the two runs are comparable
            splitprocess-phase1.log       PHASE 1's copy of the same arm, which is
                                          the evidence the reference reproduced
                                          immediately before the holder test
        """
        staged_dir = result["observations"]["staging_dir"]
        archived = {}

        holder_log = os.path.join(staged_dir, HOLDER_LOG_NAME)
        if reporter is not None and os.path.isfile(holder_log):
            try:
                archived["holder"] = reporter.archive(holder_log, "holder.log")
            except Exception as exc:              # noqa: BLE001
                self.log("experiment SPLITPROCESS_ISOLATION: could not archive the holder "
                         "log (%s)" % exc)

        phase1_log = os.path.join(staged_dir, PHASE1_LOG_NAME)
        if reporter is not None and os.path.isfile(phase1_log):
            try:
                archived["phase1"] = reporter.archive(phase1_log,
                                                      "splitprocess-phase1.log")
            except Exception as exc:              # noqa: BLE001
                self.log("experiment SPLITPROCESS_ISOLATION: could not archive the PHASE 1 "
                         "log (%s)" % exc)

        # PROCESS B's own log, named exactly as the PROCESSCONTEXT node names the
        # SINGLE arm's log. Only present when PROCESS B actually ran.
        if result.get("process_b_ran"):
            single_log = os.path.join(staged_dir, "context-%s.log"
                                      % experiments.PROCESSCONTEXT_REFERENCE_ARM)
            dest = self._archive_arm_log(reporter, single_log,
                                         experiments.PROCESSCONTEXT_REFERENCE_ARM)
            if dest:
                archived["single"] = dest

        result["observations"]["logs_archived"] = archived
        if archived:
            self.log("experiment SPLITPROCESS_ISOLATION: archived %s"
                     % ", ".join(sorted(os.path.basename(v) for v in archived.values())))
        return archived

    # -------------------------------------------------------------- archiving
    def _archive_arm_log(self, reporter, path, arm):
        if reporter is None or not os.path.isfile(path):
            return None
        try:
            return reporter.archive(path, "processcontext-%s.log" % arm)
        except Exception as exc:                      # noqa: BLE001
            self.log("experiment PROCESSCONTEXT: could not archive %s (%s)" % (path, exc))
            return None

    def archive_all_arm_logs(self, staged, reporter, result):
        """Copy every context-<mode>.log the run produced into the results tree.

        All four after a full run; only SINGLE when the gate stopped the run,
        because then the other three logs do not exist. A log that is not there
        is not an archive failure - it is the gate having worked.
        """
        archived = {}
        for arm in experiments.PROCESSCONTEXT_ARMS:
            path = os.path.join(staged["dir"], "context-%s.log" % arm)
            dest = self._archive_arm_log(reporter, path, arm)
            if dest:
                archived[arm] = dest
        result["observations"]["arm_logs_archived"] = archived
        if archived:
            self.log("experiment PROCESSCONTEXT: archived %d arm log(s): %s"
                     % (len(archived), ", ".join(sorted(os.path.basename(v)
                                                        for v in archived.values()))))
        return archived


# ---------------------------------------------------------------------------
# Module-level entry points, shared by the live runner and the dry run. The dry
# run parses the SAME fixture logs through the SAME code, so what it exercises
# is the real path and not a copy of it.
# ---------------------------------------------------------------------------

def parse_arm_logs(log_dir, arms=None, out=None):
    """Parse `context-<mode>.log` for the named arms (default: all four).

    A missing log means the arm did not run: valid=None, reference_ok=None. It
    is never reported as a failure, because an arm that never ran is not
    evidence about the variable it was supposed to introduce.

    Returns the parsed arms, and also merges them into `out` when given, so a
    two-phase run can accumulate the SINGLE gate result and the other three
    without re-reading anything.
    """
    result = {}
    for arm in (arms or experiments.PROCESSCONTEXT_ARMS):
        path = os.path.join(log_dir, "context-%s.log" % arm)
        if not os.path.isfile(path):
            result[arm] = {
                "mode": arm, "valid": None, "reference_ok": None,
                "reason": "NO_LOG", "log_path": path, "log_present": False,
                "note": "the arm produced no log file at all",
            }
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            parser = log_parser.parse_processcontext_log(fh.read(), arm)
        summary = parser.summary()
        summary["log_path"] = path
        summary["log_present"] = True

        ok, failing = experiments.gate_passes(
            experiments.PROCESSCONTEXT_REFERENCE_GATE, summary)
        # An arm can only satisfy the reference gate if it also ran to a valid
        # result: a log whose own PCAB-RESULT line says valid=NO cannot be a
        # reproduced reference, whatever its status fields happen to hold.
        if summary.get("valid") is not True:
            ok = False
            failing = list(failing) + [
                {"field": "valid", "op": "eq", "value": True,
                 "why": "the arm's own result line says valid=%s" % summary.get("valid")}]
        conflicts = summary.get("parse_conflicts") or []
        if conflicts:
            ok = False
            failing = list(failing) + [
                {"field": "parse_conflicts", "op": "eq", "value": [],
                 "why": "; ".join(conflicts)}]
        summary["reference_ok"] = ok
        summary["reference_gate_failing"] = failing
        result[arm] = summary
    if out is not None:
        out.update(result)
    return result


def build_result(arms, artifact_meta, outputs, table, seconds):
    """Build the common result object from the four parsed arms."""
    single = arms.get(experiments.PROCESSCONTEXT_REFERENCE_ARM, {})
    all_ran = all(arms.get(a, {}).get("valid") is True
                  for a in experiments.PROCESSCONTEXT_ARMS)
    conflicts = {a: arms[a].get("parse_conflicts")
                 for a in experiments.PROCESSCONTEXT_ARMS
                 if arms.get(a, {}).get("parse_conflicts")}

    if conflicts:
        valid, reason = False, "PARSE_CONFLICT: " + "; ".join(
            "%s: %s" % (a, ", ".join(c)) for a, c in sorted(conflicts.items()))
    elif not all_ran:
        missing = [a for a in experiments.PROCESSCONTEXT_ARMS
                   if arms.get(a, {}).get("valid") is not True]
        valid = False
        reason = "arms that did not run to a valid result: %s" % (", ".join(missing),)
    else:
        valid = True
        reason = "all four arms ran and produced a result line"

    return {
        "experiment": "PROCESSCONTEXT",
        "valid": valid,
        "reason": reason,
        # There is no P1.0c/P4.1 split in a standalone diagnostic. The SINGLE
        # arm - the reference lane - is reported as p1 so that the common result
        # shape is filled, and p4 is left unobserved rather than duplicated.
        # This mapping is stated here rather than inferred later.
        "p1": {
            "init": single.get("core_init"),
            "descriptor": single.get("descriptor_status"),
            "cumodule": single.get("cumodule_status"),
            "create_feature": single.get("feature"),
        },
        "p1_source": "PROCESSCONTEXT/SINGLE (there is no P4 in a standalone diagnostic)",
        "p4": {"init": None, "descriptor": None, "cumodule": None,
               "create_feature": None},
        "verdict": "",
        "next": "",
        # THE FULL arm summaries, not just the gate verdicts. Every consumer -
        # the table rows, the evidence lines and the SAME-PROCESS vs SPLIT-PROCESS
        # comparison - reads statuses from here, and a stripped-down copy made
        # them all report NOT_OBSERVED while the gate itself was satisfied.
        "arms": {a: dict(arms.get(a, {})) for a in experiments.PROCESSCONTEXT_ARMS},
        "observations": {
            "artifact": artifact_meta,
            "arm_processes": outputs,
            "table_returncode": (table or {}).get("returncode"),
            "seconds": seconds,
            "blob_size": single.get("blob_size"),
            "descriptor_handle": single.get("descriptor_handle"),
            "feature_handle": single.get("feature_handle"),
            "alloc": single.get("alloc"),
            "reference_gate": "experiments.PROCESSCONTEXT_REFERENCE_GATE",
        },
    }
