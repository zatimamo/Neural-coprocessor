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

NOTHING IS DEPLOYED
    The diagnostic runs from a staging directory under the run's results tree.
    The game's add-on is untouched by this node, and Cyberpunk is never launched.
"""

import os
import shutil
import subprocess
import time

import experiments
import log_parser
from github_actions import sha256_file


class ProcessContextError(Exception):
    pass


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
        for name in ("processcontext_ab.exe", LAUNCHER_NAME, "README.txt"):
            src = os.path.join(payload, name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(staging_dir, name))

        return {"dir": staging_dir, "exe": os.path.join(staging_dir, "processcontext_ab.exe"),
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
        "arms": {
            a: {
                "valid": arms.get(a, {}).get("valid"),
                "reference_ok": arms.get(a, {}).get("reference_ok"),
                "reason": arms.get(a, {}).get("reason"),
                "control": arms.get(a, {}).get("control"),
                "log_present": arms.get(a, {}).get("log_present"),
            } for a in experiments.PROCESSCONTEXT_ARMS
        },
        "observations": {
            "arms": arms,
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
