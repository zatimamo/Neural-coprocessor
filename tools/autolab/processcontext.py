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

NOTHING IS DEPLOYED
    The diagnostic runs from the cache directory. The NR runtime is COPIED into
    that directory from the game install - the game's own copy is only read, and
    is never replaced.
"""

import os
import shutil
import subprocess
import time

import experiments
import log_parser


class ProcessContextError(Exception):
    pass


#: The name the executable must run under. The snippet inspects its caller's
#: module file name and refuses anything without this substring.
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

    # -------------------------------------------------------------- staging
    def stage(self, artifact_meta, staging_dir):
        """Lay out the artifact plus the NR runtime in a run directory."""
        # The artifact's payload lives under `_pcab/`.
        artifact_dir = artifact_meta["artifact_dir"]
        payload = os.path.join(artifact_dir, "_pcab")
        if not os.path.isdir(payload):
            payload = artifact_dir
        exe = os.path.join(payload, "processcontext_ab.exe")
        if not os.path.isfile(exe):
            raise ProcessContextError(
                "the processcontext artifact does not contain processcontext_ab.exe "
                "(looked in %s)" % payload)

        os.makedirs(staging_dir, exist_ok=True)
        for name in ("processcontext_ab.exe", "RUN-CONTEXT-AB.cmd", "README.txt"):
            src = os.path.join(payload, name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(staging_dir, name))

        # The NR runtime, copied OUT of the game install. The game's copy is
        # read-only as far as AutoLab is concerned.
        nr_src = os.path.join(self.install_dir, self.nr_dll_rel)
        if not os.path.isfile(nr_src):
            raise ProcessContextError(
                "the diagnostic needs the NR runtime and it is not at %s" % nr_src)
        nr_dst = os.path.join(staging_dir, "nvngx_dlssnr.dll")
        shutil.copy2(nr_src, nr_dst)

        gated = os.path.join(staging_dir, GATE_PREFIX + "processcontext_ab.exe")
        shutil.copy2(exe, gated)
        return {"dir": staging_dir, "exe": gated, "nr_dll": nr_dst}

    # ------------------------------------------------------------------ run
    def run_arms(self, staged):
        """Run the four arms, each as a fresh process. Returns per-arm output."""
        outputs = {}
        for arm in experiments.PROCESSCONTEXT_ARMS:
            started = time.time()
            cmd = [staged["exe"], "--mode", arm, "--nr-dll", staged["nr_dll"]]
            self.log("experiment PROCESSCONTEXT/%s: running" % arm)
            try:
                proc = subprocess.run(cmd, cwd=staged["dir"], capture_output=True,
                                      text=True, timeout=self.timeout,
                                      errors="replace")
                out = proc.stdout or ""
                err = proc.stderr or ""
                rc = proc.returncode
            except subprocess.TimeoutExpired as exc:
                out = (exc.stdout or b"").decode("utf-8", "replace") \
                    if isinstance(exc.stdout, bytes) else (exc.stdout or "")
                err = "TIMEOUT after %ss" % self.timeout
                rc = None
            except OSError as exc:
                out, err, rc = "", "could not start the arm: %s" % exc, None
            outputs[arm] = {"returncode": rc, "stdout": out, "stderr": err,
                            "seconds": round(time.time() - started, 2)}
            self.log("experiment PROCESSCONTEXT/%s: exit %s in %.1fs"
                     % (arm, rc, outputs[arm]["seconds"]))
        return outputs

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
    def run(self, artifact_meta, run_dir):
        """Stage, run the four arms, parse, and return the result object."""
        staging = os.path.join(run_dir, "processcontext")
        staged = self.stage(artifact_meta, staging)
        started = time.time()
        outputs = self.run_arms(staged)
        table = self.run_table(staged)
        arms = self.parse_arms(staged, outputs)
        seconds = round(time.time() - started, 2)
        result = self.result_from_arms(arms, artifact_meta, outputs, table, seconds)
        result["observations"]["staging_dir"] = staging
        result["observations"]["table_stdout"] = (table or {}).get("stdout", "")
        return result


# ---------------------------------------------------------------------------
# Module-level entry points, shared by the live runner and the dry run. The dry
# run parses the SAME fixture logs through the SAME code, so what it exercises
# is the real path and not a copy of it.
# ---------------------------------------------------------------------------

def parse_arm_logs(log_dir):
    """Parse `context-<mode>.log` for each arm in log_dir.

    A missing log means the arm did not run: valid=None, reference_ok=None. It
    is never reported as a failure, because an arm that never ran is not
    evidence about the variable it was supposed to introduce.
    """
    arms = {}
    for arm in experiments.PROCESSCONTEXT_ARMS:
        path = os.path.join(log_dir, "context-%s.log" % arm)
        if not os.path.isfile(path):
            arms[arm] = {
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
        arms[arm] = summary
    return arms


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
            "reference_gate": "experiments.PROCESSCONTEXT_REFERENCE_GATE",
        },
    }
