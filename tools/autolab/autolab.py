#!/usr/bin/env python3
"""MGPU-AutoLab - a local, deterministic experiment orchestrator.

WHAT IT REPLACES
    Triggering GitHub builds, downloading artifacts, verifying hashes,
    deploying add-ons, clearing logs, launching Cyberpunk, waiting for the
    failure, grepping ReShade.log, archiving results, and deciding which
    already-defined experiment comes next.

WHAT IT DOES
    Runs the predefined experiment tree until one of exactly four things
    happens: a decisive causal result is reached, a run is invalid, the tree is
    exhausted, or an unexpected/mixed state occurs. Then it restores the
    original add-on and writes results/<timestamp>/.

WHAT IT MAY NOT DO
    Invent a hypothesis. The decisions live in experiments.py as data, and the
    controller has no branch that could express a new one. A result that matches
    no declared rule is UNEXPECTED_MIXED_STATE and AutoLab stops.

USAGE
    RUN-AUTOLAB.cmd                 the whole tree, one command
    python autolab.py --list        show the graph and its rules
    python autolab.py --parser-tests
    python autolab.py --dry-run
    python autolab.py --preflight-only
    python autolab.py --only PROCESSCONTEXT

    The game is never launched unless safety.allow_game_launch is true in
    config.yaml or --allow-game-launch is passed.
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import cyberpunk              # noqa: E402
import deployment             # noqa: E402
import experiments            # noqa: E402
import github_actions         # noqa: E402
import log_parser             # noqa: E402
import processcontext         # noqa: E402
import report                 # noqa: E402


class AutolabError(Exception):
    pass


class ConfigError(AutolabError):
    pass


class PreflightError(AutolabError):
    pass


# ===========================================================================
# Console
# ===========================================================================

class Console(object):
    """Every line the user sees is `[AUTOLAB] ...` and is also kept, so the
    report can carry the same account the console showed."""

    def __init__(self):
        self.lines = []

    def __call__(self, message):
        line = "[AUTOLAB] %s" % message
        sys.stdout.write(line + "\n")
        sys.stdout.flush()
        self.lines.append(line)

    def raw(self, text):
        sys.stdout.write(text + "\n")
        sys.stdout.flush()


# ===========================================================================
# config.yaml - a deliberately small YAML subset
# ===========================================================================

def _strip_comment(line):
    out = []
    quote = None
    for ch in line:
        if quote:
            out.append(ch)
            if ch == quote:
                quote = None
        elif ch in "'\"":
            quote = ch
            out.append(ch)
        elif ch == "#":
            break
        else:
            out.append(ch)
    return "".join(out)


def _scalar(text):
    t = text.strip()
    if t == "":
        return None
    if t == "[]":
        return []
    if t == "{}":
        return {}
    if len(t) >= 2 and t[0] in "'\"" and t[-1] == t[0]:
        return t[1:-1]
    low = t.lower()
    if low == "true":
        return True
    if low == "false":
        return False
    if low in ("null", "none", "~"):
        return None
    try:
        return int(t, 10)
    except ValueError:
        pass
    try:
        return float(t)
    except ValueError:
        pass
    return t


def _parse_block(items, i, indent):
    if i >= len(items):
        return {}, i
    first = items[i][1]
    if first == "-" or first.startswith("- "):
        out = []
        while i < len(items) and items[i][0] == indent and (
                items[i][1] == "-" or items[i][1].startswith("- ")):
            text = items[i][1][1:].strip()
            i += 1
            if text == "":
                if i < len(items) and items[i][0] > indent:
                    sub, i = _parse_block(items, i, items[i][0])
                    out.append(sub)
                else:
                    out.append(None)
            else:
                out.append(_scalar(text))
        return out, i

    out = {}
    while i < len(items) and items[i][0] == indent:
        key, sep, rest = items[i][1].partition(":")
        if not sep:
            raise ConfigError("line %d: expected 'key: value', found %r"
                              % (items[i][2], items[i][1]))
        key = key.strip()
        rest = rest.strip()
        lineno = items[i][2]
        i += 1
        if rest == "":
            if i < len(items) and items[i][0] > indent:
                sub, i = _parse_block(items, i, items[i][0])
                out[key] = sub
            else:
                out[key] = None
        else:
            out[key] = _scalar(rest)
        if not key:
            raise ConfigError("line %d: empty key" % lineno)
    return out, i


def load_config(path):
    if not os.path.isfile(path):
        raise ConfigError("config file not found: %s" % path)
    with open(path, "r", encoding="utf-8") as fh:
        raw = fh.readlines()
    items = []
    for n, line in enumerate(raw, 1):
        stripped = line.lstrip(" ")
        if stripped and line[:len(line) - len(stripped)].find("\t") >= 0:
            raise ConfigError("line %d: tabs are not allowed for indentation" % n)
        body = _strip_comment(line).rstrip()
        if not body.strip():
            continue
        indent = len(body) - len(body.lstrip(" "))
        items.append((indent, body.strip(), n))
    if not items:
        return {}
    value, idx = _parse_block(items, 0, items[0][0])
    if idx != len(items):
        raise ConfigError("line %d: unexpected indentation" % items[idx][2])
    if not isinstance(value, dict):
        raise ConfigError("the config file must be a mapping at the top level")
    return value


def repo_root():
    return os.path.dirname(os.path.dirname(HERE))


def resolve_dir(cfg, key):
    value = cfg.get(key) or key
    if os.path.isabs(value):
        return value
    return os.path.abspath(os.path.join(repo_root(), value))


def require(cfg, *path):
    cur = cfg
    for part in path:
        if not isinstance(cur, dict) or part not in cur:
            raise ConfigError("config is missing %s" % ".".join(path))
        cur = cur[part]
    if cur is None:
        raise ConfigError("config value %s is empty" % ".".join(path))
    return cur


# ===========================================================================
# Formatting helpers - the only place values become text
# ===========================================================================

def _dec(value):
    return "NOT_OBSERVED" if value is None else str(value)


def _hex(value):
    return "NOT_OBSERVED" if value is None else "0x%08X" % (value & 0xFFFFFFFF)


def _hexptr(value):
    return "NOT_OBSERVED" if value is None else "0x%X" % value


def _ngx_name(code):
    if code is None:
        return "NOT_OBSERVED"
    known = {0x00000001: "Success", 0xBAD00001: "FAIL_InvalidParameter",
             0xBAD00002: "FAIL_PlatformError", 0xBAD00003: "FAIL_NotInitialized",
             0xBAD00005: "FAIL_InvalidState", 0xBAD0000B: "FAIL_UnableToInitializeFeature",
             0xBAD0000C: "FAIL_OutOfDate", 0xBAD00012: "FAIL_FeatureRequirementsQueryFailed"}
    return("%s (%s)" % (_hex(code), known.get(code, "see nvsdk_ngx_defs.h")))


# ===========================================================================
# Live console status
# ===========================================================================

class LiveStatus(object):
    """Prints a field the first time it is observed, and every change after."""

    FIELDS = [
        ("P1 Init", lambda p: p.p1_core_init),
        ("P4 Init", lambda p: p.p4_core_init),
        ("Descriptor", lambda p: p.first_real_status(p.descriptor_calls)),
        ("CuModule", lambda p: p.first_real_status(p.cumodule_calls)),
        ("Reserved18", lambda p: p.p1_create_feature),
    ]

    def __init__(self, console, parser):
        self.console = console
        self.parser = parser
        self._last = {}

    def update(self):
        for label, getter in self.FIELDS:
            try:
                value = getter(self.parser)
            except Exception:
                value = None
            if value is None or self._last.get(label) == value:
                continue
            self._last[label] = value
            if label in ("P1 Init", "P4 Init", "Reserved18"):
                self.console("%s = %s" % (label, _ngx_name(value)))
            else:
                self.console("%s = %s" % (label, _dec(value)))

    def finish(self):
        """Guarantee all five lines appear, with NOT_OBSERVED where they must."""
        for label, getter in self.FIELDS:
            if label in self._last:
                continue
            self.console("%s = NOT_OBSERVED" % label)


# ===========================================================================
# Preflight
# ===========================================================================

def preflight(cfg, console, reporter, deploy, cp, gh, run_dir):
    """Every check, in order. NOTHING outside the results directory is written
    until every read-only check has passed."""
    console("preflight")

    def ok(label, detail=""):
        console("preflight: %s%s" % (label, (" - " + detail) if detail else ""))

    exe = require(cfg, "paths", "cyberpunk_exe")
    if not os.path.isfile(exe):
        raise PreflightError("the Cyberpunk executable is not there: %s" % exe)
    ok("Cyberpunk executable present", exe)

    install = require(cfg, "paths", "install_dir")
    if not os.path.isdir(install):
        raise PreflightError("the install directory is not there: %s" % install)
    ok("install directory present", install)

    addon = deploy.addon_path
    if not os.path.isfile(addon):
        raise PreflightError(
            "no MGPU add-on at %s. AutoLab cannot back up or restore an add-on "
            "that is not installed." % addon)
    installed_sha = deployment.sha256_file(addon)
    ok("installed add-on present", "%s (%s)" % (os.path.basename(addon), installed_sha))

    nr_path = deploy.nr_dll_path
    if not os.path.isfile(nr_path):
        raise PreflightError("the NR runtime is not there: %s" % nr_path)
    nr_sha = deploy.verify_nr_dll()
    ok("NR runtime hash matches the required NeuralScreen build", nr_sha.upper())

    # THE NEURALSCREEN REFERENCE RUNTIME. This is the copy the PROCESSCONTEXT
    # node runs against, and its DIRECTORY is the NGX data path the diagnostic
    # derives - so it is located and verified here, before CI is asked to do
    # anything, and a wrong or missing one is refused before anything is built.
    ns_nr = deploy.ns_nr_dll_path
    if not ns_nr:
        raise PreflightError(
            "paths.neuralscreen_nr_dll is not configured. The PROCESSCONTEXT node "
            "runs the reference lane and needs the NeuralScreen runtime, not the "
            "game's copy.")
    if not os.path.isfile(ns_nr):
        raise PreflightError("the NeuralScreen reference runtime is not there: %s" % ns_nr)
    ns_sha = deploy.verify_ns_nr_dll()
    ok("NeuralScreen reference runtime hash matches", ns_sha.upper())
    ok("NeuralScreen reference data path", os.path.dirname(ns_nr))

    if cp.is_running():
        raise PreflightError("Cyberpunk is already running (pids %s). Close it and re-run."
                             % (cp.running_pids(),))
    ok("Cyberpunk is not already running")

    auth = gh.auth_check()
    ok("GitHub access", auth)

    # ---- first write: the results tree -----------------------------------
    try:
        os.makedirs(run_dir, exist_ok=True)
        probe = os.path.join(run_dir, ".writable")
        with open(probe, "w", encoding="utf-8") as fh:
            fh.write("ok")
        os.remove(probe)
    except OSError as exc:
        raise PreflightError("the results directory is not writable: %s (%s)"
                             % (run_dir, exc))
    ok("results directory writable", run_dir)

    # ---- backup: the only thing that makes the run undoable --------------
    backup_dir = os.path.join(run_dir, "backup")
    backup = deploy.backup(backup_dir)
    if deployment.sha256_file(backup["path"]) != backup["sha256"]:
        raise PreflightError("the add-on backup did not verify")
    ok("working backup of the installed add-on", backup["path"])

    reporter.hash("addon.installed.at_preflight", installed_sha, path=addon)
    reporter.hash("addon.backup", backup["sha256"], path=backup["path"])
    reporter.hash("nr_dll", nr_sha, path=nr_path,
                  expected=cfg["required_nr_dll_sha256"])
    reporter.hash("nr_dll.neuralscreen_reference", ns_sha, path=ns_nr,
                  expected=cfg["required_nr_dll_sha256"])
    console("preflight OK")
    return {"installed_sha256": installed_sha, "backup": backup, "nr_dll_sha256": nr_sha,
            "ns_nr_dll_sha256": ns_sha, "ns_nr_dll_path": ns_nr,
            "auth": auth}


# ===========================================================================
# Experiment runners
# ===========================================================================

def run_processcontext_node(name, spec, cfg, console, reporter, gh, deploy, run_dir,
                            use_cache=True):
    console("experiment %s" % name)
    wf = experiments.WORKFLOWS[spec["workflow"]]
    meta = gh.build(name, wf["workflow"], wf["artifact"],
                    "_pcab/processcontext_ab.exe", reuse_cache=use_cache)
    reporter.event("build", experiment=name, run_id=meta.get("run_id"),
                   commit=meta.get("commit"), artifact=meta.get("artifact_name"),
                   from_cache=bool(meta.get("from_cache")),
                   addon_sha256=meta.get("addon_sha256"))
    reporter.hash("artifact.processcontext_ab.exe", meta["addon_sha256"],
                  path=meta["addon_path"])

    runner = processcontext.ProcessContextRunner(cfg, console, gh)
    # The reporter goes in so the SINGLE gate log can be kept aside the moment
    # it is read, before the second phase can overwrite it.
    result = runner.run(meta, run_dir, reporter=reporter)

    # ALL FOUR arm logs, archived together once the run is over.
    runner.archive_all_arm_logs({"dir": result["observations"]["staging_dir"]},
                                reporter, result)
    for arm, path in sorted((result["observations"].get("arm_logs_archived") or {}).items()):
        reporter.hash("processcontext.%s.log" % arm, deployment.sha256_file(path),
                      path=path)

    single = result["arms"].get(
        experiments.PROCESSCONTEXT_REFERENCE_ARM, {})
    console("P1 Init = %s" % _ngx_name(single.get("core_init")))
    console("Descriptor = %s" % _dec(single.get("descriptor_status")))
    console("CuModule = %s" % _dec(single.get("cumodule_status")))
    console("Reserved18 = %s" % _ngx_name(single.get("feature")))
    console("Blob size = %s" % _dec(single.get("blob_size")))
    console("Reference handle = %s" % _hexptr(single.get("descriptor_handle")))
    for arm in experiments.PROCESSCONTEXT_ARMS:
        a = result["arms"].get(arm, {})
        console("%s: valid=%s reference_ok=%s" % (arm, a.get("valid"),
                                                  a.get("reference_ok")))
    return result, meta


def run_game_node(name, spec, cfg, console, reporter, gh, deploy, cp, run_dir,
                  use_cache=True):
    console("experiment %s" % name)
    # THE LAUNCH GATE IS CHECKED FIRST, before the build and before the deploy.
    # A run that cannot be launched must not write the add-on into the game
    # directory at all - refusing after deploying would leave an experimental
    # build installed for no reason.
    if not cp.allow_launch:
        raise cyberpunk.GameLaunchNotPermitted(
            "%s needs the game and game launch is disabled. Set "
            "safety.allow_game_launch: true in config.yaml, or pass "
            "--allow-game-launch. Nothing has been built or deployed." % name)

    wf = experiments.WORKFLOWS[spec["workflow"]]
    meta = gh.build(name, wf["workflow"], wf["artifact"],
                    experiments.ADDON_IN_ARTIFACT, reuse_cache=use_cache)
    reporter.event("build", experiment=name, run_id=meta.get("run_id"),
                   commit=meta.get("commit"), artifact=meta.get("artifact_name"),
                   from_cache=bool(meta.get("from_cache")),
                   addon_sha256=meta.get("addon_sha256"))
    reporter.hash("artifact.addon", meta["addon_sha256"], path=meta["addon_path"])

    console("deploy %s%s" % (name, " (from cache)" if meta.get("from_cache") else ""))
    installed = deploy.deploy(meta["addon_path"])
    if installed["sha256"] != meta["addon_sha256"]:
        raise AutolabError(
            "the installed add-on hash %s does not match the artifact %s"
            % (installed["sha256"], meta["addon_sha256"]))
    reporter.hash("addon.deployed", installed["sha256"], path=installed["path"],
                  expected=meta["addon_sha256"])
    deploy.verify_nr_dll()

    cp.clear_log()
    console("launching %s" % name)
    pid = cp.launch()
    reporter.event("launch", experiment=name, pid=pid)

    parser = log_parser.ReShadeLogParser()
    live = LiveStatus(console, parser)
    status, elapsed = cp.tail(
        parser,
        lambda p: experiments.required_complete(name, p),
        on_line=lambda _line: live.update())
    live.update()
    live.finish()
    console("%s: tail ended %s after %.1fs" % (name, status, elapsed))

    # The graceful close is tried first, for every run. Only after the wait does
    # a forced termination follow.
    closed = cp.close_main_window()
    wait = int(cfg["timeouts"]["post_close_wait_seconds"])
    deadline = time.time() + wait
    while time.time() < deadline and cp.is_running():
        time.sleep(0.5)
    forced = []
    if cp.is_running():
        forced = cp.terminate()
    console("%s: closed=%s force_terminated=%s" % (name, closed, forced or "no"))

    archived = cp.archive_log(reporter.logs_dir, "reshade-%s.log" % name)
    run_info = {
        "tail_status": status,
        "tail_seconds": round(elapsed, 2),
        "launch_pid": pid,
        "close_posted": closed,
        "force_terminated": forced,
        "log_archived": archived,
        "game_seconds_limit": int(cfg["timeouts"]["game_run_seconds"]),
    }
    reporter.event("run", experiment=name, **run_info)
    if archived:
        reporter.hash("reshade.log.archived", deployment.sha256_file(archived),
                      path=archived)

    result = experiments.build_game_result(name, parser, meta, run_info)
    if status == "timeout":
        # A timeout is INVALID/TIMEOUT. It is never a result.
        result["valid"] = False
        result["reason"] = ("INVALID/TIMEOUT: the required observations did not all "
                            "appear within %ss. %s"
                            % (run_info["game_seconds_limit"], result["reason"]))
    return result, meta


def run_splitprocess_node(name, spec, cfg, console, reporter, gh, deploy, cp, run_dir,
                          use_cache=True):
    """PROCESS A holds Ti SUPER state; PROCESS B is the existing SINGLE arm.

    No game is involved and the add-on is not touched. The artifact is the SAME
    processcontext-ab artifact the PROCESSCONTEXT node uses, and PROCESS B is the
    same SINGLE arm through the same launcher - so the only thing that differs
    from the reference run is which process holds the second adapter's state.
    """
    console("experiment %s" % name)
    wf = experiments.WORKFLOWS[spec["workflow"]]
    meta = gh.build(name, wf["workflow"], wf["artifact"],
                    "_pcab/processcontext_ab.exe", reuse_cache=use_cache)
    reporter.event("build", experiment=name, run_id=meta.get("run_id"),
                   commit=meta.get("commit"), artifact=meta.get("artifact_name"),
                   from_cache=bool(meta.get("from_cache")),
                   addon_sha256=meta.get("addon_sha256"))
    reporter.hash("artifact.processcontext_ab.exe", meta["addon_sha256"],
                  path=meta["addon_path"])

    runner = processcontext.ProcessContextRunner(cfg, console, gh)
    result = runner.run_split(meta, run_dir, reporter=reporter)
    runner.archive_split_logs(result, reporter)

    # PHASE 1 first: the reference control in THIS environment, before the
    # holder test means anything.
    obs = result["observations"]
    phase1 = (obs.get("phase1") or {})
    p1 = phase1.get("summary") or {}
    console("PHASE 1 reference-alone: reference_ok = %s" % p1.get("reference_ok"))
    if not phase1.get("passed"):
        console("PHASE 1 DID NOT REPRODUCE: %s" % (phase1.get("detail") or "no result"))
        console("%s was NOT launched" % processcontext.HOLDER_EXE_NAME)
    else:
        holder = obs["holder"]
        console("PHASE 2 PROCESS A %s:" % processcontext.HOLDER_EXE_NAME)
        console("  HOLDER_READY = %s" % ("yes" if holder["ready"] else "no"))
        console("  holder alive after READY = %s" % holder["alive_after_ready"])
        console("  HOLDER_STOPPED = %s" % ("yes" if holder.get("stopped_line") else "no"))
        console("  holder exit = %s" % holder["returncode"])
        sha = obs.get("executable_sha256") or {}
        console("  PROCESS B executable sha256 identical in both phases = %s"
                % sha.get("identical"))
    single = result["arms"].get(experiments.PROCESSCONTEXT_REFERENCE_ARM, {})
    console("PROCESS B launched = %s" % result.get("process_b_ran"))
    console("Descriptor = %s" % _dec(single.get("descriptor_status")))
    console("CuModule = %s" % _dec(single.get("cumodule_status")))
    console("Reserved18 = %s" % _ngx_name(single.get("feature")))
    console("PROCESS B reference_ok = %s" % single.get("reference_ok"))
    return result, meta


def run_node(name, spec, cfg, console, reporter, gh, deploy, cp, run_dir, use_cache=True):
    if spec["workflow"] == "PROCESSCONTEXT" and name == "SPLITPROCESS_ISOLATION":
        return run_splitprocess_node(name, spec, cfg, console, reporter, gh, deploy, cp,
                                     run_dir, use_cache)
    if spec["workflow"] == "PROCESSCONTEXT":
        return run_processcontext_node(name, spec, cfg, console, reporter, gh, deploy,
                                       run_dir, use_cache)
    return run_game_node(name, spec, cfg, console, reporter, gh, deploy, cp, run_dir,
                         use_cache)


# ===========================================================================
# Report assembly
# ===========================================================================

def build_rows(results, imported):
    rows = []
    for imp in imported:
        rows.append({"experiment": imp["experiment"], "valid": imp["valid"],
                     "result": imp["verdict"], "note": imp.get("reason")})
    for res in results:
        kind = res.get("kind") or ("splitprocess" if _is_split_result(res)
                                   else "procescontext")
        if kind == "splitprocess":
            # PHASE 1 is the current-environment reference control: this exact
            # executable, alone, immediately before the holder test.
            phase1 = (res.get("observations", {}).get("phase1") or {})
            rows.append({"experiment": "SPLIT/PHASE1",
                         "valid": res.get("phase1_ok"),
                         "result": (report.RESULT_REFERENCE_OK if res.get("phase1_ok")
                                    else report.RESULT_REFERENCE_FAILED
                                    if res.get("phase1_ok") is False
                                    else report.RESULT_NOT_RUN)})
            # The holder is the arm this node adds; its "valid" is whether it
            # established the state it exists to hold AND reported the clean stop.
            rows.append({"experiment": "SPLIT/HOLDER",
                         "valid": res.get("holder_established"),
                         "result": ("HOLDER_READY+HOLDER_STOPPED"
                                    if res.get("holder_established") and res.get("holder_stopped")
                                    else "HOLDER_READY, NO HOLDER_STOPPED"
                                    if res.get("holder_established")
                                    else "HOLDER_NOT_ESTABLISHED")})
            single = res.get("arms", {}).get(
                experiments.PROCESSCONTEXT_REFERENCE_ARM, {})
            if not res.get("process_b_ran"):
                text = "%s (PROCESS B not launched)" % report.RESULT_NOT_RUN
            elif single.get("reference_ok") is True:
                text = report.RESULT_REFERENCE_OK
            elif single.get("reference_ok") is False:
                text = report.RESULT_REFERENCE_FAILED
            else:
                text = report.RESULT_NOT_RUN
            rows.append({"experiment": "SPLIT/SINGLE", "valid": single.get("valid"),
                         "result": text, "note": phase1.get("detail") or None})
            if not res.get("exe_same_bytes", True):
                rows.append({"experiment": "SPLIT/EXE-SHA256", "valid": False,
                             "result": "THE EXECUTABLE CHANGED BETWEEN PHASE 1 AND PHASE 2"})
        elif "arms" in res:
            for arm in experiments.PROCESSCONTEXT_ARMS:
                a = res.get("arms", {}).get(arm, {})
                if a.get("reference_ok") is True:
                    text = report.RESULT_REFERENCE_OK
                elif a.get("reference_ok") is False:
                    text = report.RESULT_REFERENCE_FAILED
                else:
                    text = report.RESULT_NOT_RUN
                rows.append({"experiment": "PROCESS/" + arm.upper(),
                             "valid": a.get("valid"), "result": text})
        rows.append({"experiment": res["experiment"], "valid": res["valid"],
                     "result": res.get("verdict") or report.RESULT_INVALID})
    return rows


def _is_split_result(res):
    """Split-process results are recognised by their SHAPE, not their name.

    The dry run renames its results so the table says which fixture each row came
    from; matching on the name would make the comparison vanish there and only
    there, which is exactly where it needs exercising.
    """
    return res.get("holder_established") is not None and "arms" in res


def _is_procescontext_result(res):
    return "arms" in res and not _is_split_result(res)


def build_comparison(results):
    """SAME PROCESS / dual-active against SPLIT PROCESS, the whole point.

    Generated only from what the two nodes actually recorded. A missing dual
    active arm, or a split run whose holder never established its state, is
    reported as MISSING/NOT_APPLICABLE rather than filled in with the value the
    architecture predicts.

    A live run produces exactly one of each. When more than one is present - the
    dry run, which walks every branch - the PROCESSCONTEXT result that actually
    shows the same-process failure is preferred, because that is the one the
    comparison is about.
    """
    pc_candidates = [r for r in results if _is_procescontext_result(r)]
    split_candidates = [r for r in results if _is_split_result(r)]
    if not pc_candidates and not split_candidates:
        return []

    pc = None
    for cand in pc_candidates:
        if cand.get("arms", {}).get("dual-active", {}).get("reference_ok") is False:
            pc = cand
            break
    if pc is None and pc_candidates:
        pc = pc_candidates[0]

    split = None
    for cand in split_candidates:
        if cand.get("holder_established"):
            split = cand
            break
    if split is None and split_candidates:
        split = split_candidates[0]

    def cell(value, fmt):
        return "NOT_OBSERVED" if value is None else fmt(value)

    lines = []
    lines.append("SAME PROCESS / dual-active, from PROCESSCONTEXT:")
    if pc is None:
        lines.append("    the PROCESSCONTEXT result is not in this run - SAME PROCESS row MISSING")
    else:
        a = pc.get("arms", {}).get("dual-active", {})
        lines.append("    Descriptor  %s" % cell(a.get("descriptor_status"), _dec))
        lines.append("    CuModule    %s" % cell(a.get("cumodule_status"), _dec))
        lines.append("    Reserved18  %s" % cell(a.get("feature"), _ngx_name))
        lines.append("    reference_ok %s (valid=%s)"
                     % (a.get("reference_ok"), a.get("valid")))

    lines.append("SPLIT PROCESS, from SPLITPROCESS_ISOLATION:")
    if split is None:
        lines.append("    the SPLITPROCESS_ISOLATION result is not in this run - SPLIT row MISSING")
    else:
        lines.append("    PHASE 1  reference-alone, the exact PROCESS B executable:")
        p1 = (split.get("observations", {}).get("phase1") or {}).get("summary") or {}
        lines.append("        Descriptor  %s" % cell(p1.get("descriptor_status"), _dec))
        lines.append("        CuModule    %s" % cell(p1.get("cumodule_status"), _dec))
        lines.append("        Reserved18  %s" % cell(p1.get("feature"), _ngx_name))
        lines.append("        reference_ok %s (valid=%s)"
                     % (p1.get("reference_ok"), p1.get("valid")))
        if not split.get("holder_established"):
            lines.append("    PHASE 2  NOT APPLICABLE: the holder did not establish its "
                         "state, so PROCESS B was not launched")
        else:
            s = split.get("arms", {}).get(experiments.PROCESSCONTEXT_REFERENCE_ARM, {})
            lines.append("    PHASE 2  Ti SUPER active in a DIFFERENT process, same arm:")
            lines.append("        Descriptor  %s" % cell(s.get("descriptor_status"), _dec))
            lines.append("        CuModule    %s" % cell(s.get("cumodule_status"), _dec))
            lines.append("        Reserved18  %s" % cell(s.get("feature"), _ngx_name))
            lines.append("        reference_ok %s (valid=%s)"
                         % (s.get("reference_ok"), s.get("valid")))
        sha = split.get("observations", {}).get("executable_sha256") or {}
        lines.append("    PROCESS B executable: PHASE 1 %s" % (sha.get("phase1") or "?").upper())
        if sha.get("phase2"):
            lines.append("                          PHASE 2 %s  (identical: %s)"
                         % (sha["phase2"].upper(), sha.get("identical")))
        else:
            lines.append("                          PHASE 2 NOT REACHED")
    return lines


def build_evidence(results, hashes):
    lines = []
    for res in results:
        lines.append("%s: valid=%s - %s" % (res["experiment"], res["valid"],
                                            res.get("reason", "")))
        if res.get("kind") == "splitprocess" or _is_split_result(res):
            obs = res.get("observations") or {}
            p1 = (obs.get("phase1") or {}).get("summary") or {}
            lines.append("  PHASE 1 reference-alone (the same executable that runs as "
                         "PROCESS B)  descriptor=%-13s cumodule=%-13s blob=%-9s "
                         "Reserved18=%-22s reference_ok=%s"
                         % (_dec(p1.get("descriptor_status")), _dec(p1.get("cumodule_status")),
                            _dec(p1.get("blob_size")), _ngx_name(p1.get("feature")),
                            p1.get("reference_ok")))
            if not (obs.get("phase1") or {}).get("passed"):
                lines.append("  PHASE 1 did not reproduce: %s"
                             % ((obs.get("phase1") or {}).get("detail") or "no result"))
            h = obs.get("holder") or {}
            lines.append("  %s (PROCESS A) ready=%s alive_after_ready=%s stopped=%s exit=%s "
                         "event=%s"
                         % (processcontext.HOLDER_EXE_NAME, h.get("ready"),
                            h.get("alive_after_ready"), h.get("stopped_line"),
                            h.get("returncode"), h.get("event_name")))
            sha = obs.get("executable_sha256") or {}
            sha2 = (sha.get("phase2") or "").upper() or "PHASE 2 NOT REACHED"
            lines.append("  PROCESS B executable sha256  phase1=%s  phase2=%s%s"
                         % ((sha.get("phase1") or "?").upper(), sha2,
                            "" if sha.get("identical") is None
                            else "  identical=%s" % sha.get("identical")))
            s = res.get("arms", {}).get(experiments.PROCESSCONTEXT_REFERENCE_ARM, {})
            launch = "launched" if res.get("process_b_ran") else "NOT LAUNCHED"
            lines.append("  PROCESS B SINGLE (%s)  descriptor=%-13s cumodule=%-13s blob=%-9s "
                         "Reserved18=%-22s handle=%-12s reference_ok=%s"
                         % (launch, _dec(s.get("descriptor_status")),
                            _dec(s.get("cumodule_status")), _dec(s.get("blob_size")),
                            _ngx_name(s.get("feature")), _hexptr(s.get("feature_handle")),
                            s.get("reference_ok")))
        elif "arms" in res:
            arms = res.get("arms") or {}
            for arm in experiments.PROCESSCONTEXT_ARMS:
                a = arms.get(arm, {})
                lines.append(
                    "  %-13s valid=%-5s descriptor=%-13s cumodule=%-13s blob=%-9s "
                    "Reserved18=%-22s handle=%-12s reference_ok=%s"
                    % (arm, a.get("valid"), _dec(a.get("descriptor_status")),
                       _dec(a.get("cumodule_status")), _dec(a.get("blob_size")),
                       _ngx_name(a.get("feature")), _hexptr(a.get("descriptor_handle")),
                       a.get("reference_ok")))
        else:
            flat = res.get("flat") or {}
            for phase in ("p1", "p4"):
                lines.append(
                    "  %-3s Init=%-22s descriptor=%-13s CuModule=%-13s Reserved18=%s"
                    % (phase.upper(), _ngx_name(flat.get("%s_init" % phase)),
                       _dec(flat.get("%s_descriptor_status" % phase)),
                       _dec(flat.get("%s_cumodule_status" % phase)),
                       _ngx_name(flat.get("%s_create_feature" % phase))))
    for rec in hashes:
        lines.append("hash %s  %s" % (rec["sha256"], rec["label"]))
    comparison = build_comparison(results)
    if comparison:
        lines.append("")
        lines.append("SAME PROCESS vs SPLIT PROCESS:")
        lines.extend("    " + c for c in comparison)
    return lines


# ===========================================================================
# Parser tests
# ===========================================================================

FIXTURES = os.path.join(HERE, "fixtures")

PARSER_TESTS = [
    {"name": "game/failing-shape (the historical failure)",
     "log": "game_failing.log",
     "expect": {"p1_core_init": 0x00000001, "p4_core_init": 0x00000001,
                "descriptor_real_status": -1, "cumodule_real_status": -1,
                "p1_create_feature": 0xBAD00002, "p4_create_feature": 0xBAD00002,
                "descriptor_real_handle": None, "cumodule_blob_size": 3944768}},
    {"name": "game/working-shape (the NeuralScreen result)",
     "log": "game_working.log",
     "expect": {"p1_core_init": 0x00000001, "p4_core_init": 0x00000001,
                "descriptor_real_status": 0, "cumodule_real_status": 0,
                "p1_create_feature": 0x00000001, "p4_create_feature": 0x00000001,
                "descriptor_real_handle": 0x1A4BAD7000,
                "cumodule_real_handle": 0xA000,
                "cumodule_blob_size": 3944768}},
    {"name": "game/probes-only (a -14 probe is NOT a failure)",
     "log": "game_probe_only.log",
     "expect": {"descriptor_real_status": None, "cumodule_real_status": None,
                "descriptor_probe_count": 1, "cumodule_probe_count": 1,
                "p1_core_init": 0x00000001, "p4_core_init": 0x00000001}},
    {"name": "game/missing-p4 (an absent observation stays absent)",
     "log": "game_missing_p4.log",
     "expect": {"p1_core_init": 0x00000001, "p4_core_init": None,
                "descriptor_real_status": -1, "cumodule_real_status": -1}},
    {"name": "game/handle-not-read (not read is not zero)",
     "log": "game_handle_not_read.log",
     "expect": {"descriptor_real_status": 0, "descriptor_real_handle": None,
                "cumodule_real_status": -1, "cumodule_real_handle": None}},
]

#: Graph scenarios: fixture directory -> the verdict the graph must reach.
#: Both multi-adapter verdicts now ROUTE to the architecture-validation node
#: rather than stopping, because in either case a second adapter's D3D12 state
#: exists inside the process that runs the RTX 4070 lane.
GRAPH_SCENARIOS = [
    ("pcab_all_ok", "MULTI_DEVICE_EXONERATED", "STREAMLINE_CONTEXT_REQUIRED"),
    ("pcab_held_fail", "SECOND_LIVE_D3D12_DEVICE_SUFFICIENT", "SPLITPROCESS_ISOLATION"),
    ("pcab_active_fail", "ACTIVE_SECOND_DEVICE_STATE_SUFFICIENT", "SPLITPROCESS_ISOLATION"),
    ("pcab_released_fail", "PROCESS_GLOBAL_STATE_PERSISTS_AFTER_DEVICE_RELEASE", None),
    ("pcab_single_fail", "PROCESSCONTEXT_REFERENCE_INVALID", None),
    # The CURRENT environment's failure: control core Init FAIL_OutOfDate. It is
    # a different failure from the dual-active signature and the graph must treat
    # it the same way - as "the reference did not reproduce".
    ("pcab_single_outofdate", "PROCESSCONTEXT_REFERENCE_INVALID", None),
    ("pcab_unexpected", "UNEXPECTED_MIXED_STATE", None),
]

#: SPLITPROCESS_ISOLATION cases.
#:
#: PHASE 1 and PHASE 2 run the SAME arm, so what varies between the cases is
#: which single-arm log the fixture hands back. `phase1_fixture` is the log the
#: stub returns for PHASE 1 (the current-environment reference control);
#: `fixture` is the log it returns for PROCESS B.
#:
#:   phase1_ok   the reference reproduced here and now
#:   holder_ok   holder_ti.exe reached HOLDER_READY and stayed alive
#:   fixture     what PROCESS B produced: ok / the same-process failure signature
#:               (descriptor -1, CuModule -1, Reserved18 0xBAD00002) / some other
#:               failure, which is NOT the same result and must not be reported
#:               as PROCESS_ISOLATION_NOT_SUFFICIENT
#:
#: `launcher_calls` is the EXACT sequence of launcher invocations, so PHASE 1 and
#: PROCESS B are separately visible: [["single"]] means PROCESS B never ran.
#: `archived` counts the logs on disk: PHASE 1's reference log always; the
#: holder's log when the holder started; PROCESS B's log when it ran.
SPLIT_CASES = [
    {"phase1_fixture": "pcab_all_ok", "phase1_ok": True,
     "fixture": "pcab_all_ok", "holder_ok": True, "holder_started": True,
     "launcher_calls": [["single"], ["single"]], "archived": 3,
     "verdict": "PROCESS_ISOLATION_VALIDATED"},
    {"phase1_fixture": "pcab_all_ok", "phase1_ok": True,
     "fixture": "pcab_single_fail", "holder_ok": True, "holder_started": True,
     "launcher_calls": [["single"], ["single"]], "archived": 3,
     "verdict": "PROCESS_ISOLATION_NOT_SUFFICIENT"},
    # PROCESS B failed, but NOT with the same-process signature: a different
    # event, so the node must refuse to call it "not sufficient".
    {"phase1_fixture": "pcab_all_ok", "phase1_ok": True,
     "fixture": "pcab_single_outofdate", "holder_ok": True, "holder_started": True,
     "launcher_calls": [["single"], ["single"]], "archived": 3,
     "verdict": "UNEXPECTED_MIXED_STATE"},
    {"phase1_fixture": "pcab_all_ok", "phase1_ok": True,
     "fixture": "pcab_all_ok", "holder_ok": False, "holder_started": True,
     "launcher_calls": [["single"]], "archived": 2,
     "verdict": "INVALID_HOLDER"},
    # The reference did not reproduce HERE, so the holder is never launched and
    # the run says nothing about process isolation. This is the current
    # environment's failure - control core Init FAIL_OutOfDate - and it is the
    # reason PHASE 1 exists at all.
    {"phase1_fixture": "pcab_single_outofdate", "phase1_ok": False,
     "fixture": "pcab_all_ok", "holder_ok": True, "holder_started": False,
     "launcher_calls": [["single"]], "archived": 1,
     "verdict": "SPLITPROCESS_REFERENCE_INVALID"},
    # The two phases did not run the same bytes. PROCESS B is not launched: the
    # comparison would be void, and no verdict about isolation is available.
    {"phase1_fixture": "pcab_all_ok", "phase1_ok": True,
     "fixture": "pcab_all_ok", "holder_ok": True, "holder_started": True,
     "change_bytes": True,
     "launcher_calls": [["single"]], "archived": 2,
     "verdict": "UNEXPECTED_MIXED_STATE"},
]


#: NEGATIVE CONTROLS.
#:
#: The tests above could pass simply because the fixtures and the parser were
#: written together. Each control below MUTATES a fixture in memory so that the
#: property under test must flip, and asserts that it does. If a control ever
#: stops flipping, the corresponding test above has stopped testing anything:
#:
#:   * a -14 probe with a REAL first argument must stop being a probe, which is
#:     what proves the classifier is not simply `status == -14`;
#:   * a line that prints an OBSERVED zero handle must yield 0, which is what
#:     proves the NOT-READ path is why the other test sees None - and not a
#:     parser that never reads a handle at all;
#:   * removing the P4.1 Init line must fail the validity gate.
NEGATIVE_CONTROLS = [
    {"name": "control/-14 descriptor probe with a REAL pParams is NOT a probe",
     "base": "game_probe_only.log",
     "replace": [("pParams=0000000000000000", "pParams=0x0000004B2FEFA180")],
     "expect": {"descriptor_probe_count": 0, "descriptor_real_status": -14}},
    {"name": "control/-14 CuModule probe with a REAL blob is NOT a probe",
     "base": "game_probe_only.log",
     "replace": [("pBlob=0000000000000000 size=0 phModule=0000000000000000",
                  "pBlob=0x000001D4A1000000 size=3944768 phModule=0x0000004B2FEFA1C0")],
     "expect": {"cumodule_probe_count": 0, "cumodule_real_status": -14}},
    {"name": "control/an OBSERVED zero handle is not the same as NOT READ",
     "base": "game_handle_not_read.log",
     "replace": [("returned handle NOT READ: structSizeOut=0 does not cover it (needs 48)",
                  "returned handle=0x0000000000000000")],
     "expect": {"descriptor_real_status": 0, "descriptor_real_handle": 0}},
    {"name": "control/a P4.1 Init removed fails the validity gate",
     "base": "game_working.log",
     "replace": [("[MGPU][P4.1] Init: result=0x00000001 (NVSDK_NGX_Result_Success)", "")],
     "expect_gate_failed": True},
    {"name": "control/a FAIL_OutOfDate P4.1 Init fails the validity gate",
     "base": "game_working.log",
     "replace": [("[MGPU][P4.1] Init: result=0x00000001 (NVSDK_NGX_Result_Success)",
                  "[MGPU][P4.1] Init: result=0xBAD0000C (FAIL_OutOfDate)")],
     "expect_gate_failed": True},
]


#: NEGATIVE CONTROLS for the REFERENCE GATE.
#:
#: The gate has eight conditions. Six of them are new: the three Init/Allocate
#: results and the feature handle. These controls mutate a PASSING fixture one
#: field at a time and assert the gate stops passing, so each condition is shown
#: to be load-bearing rather than decorative.
REFERENCE_GATE_CONTROLS = [
    {"name": "gate/core Init not Success fails the reference",
     "base": "pcab_all_ok/context-single.log",
     "replace": [("core_init=0x00000001", "core_init=0x00000000")]},
    {"name": "gate/AllocateParameters not Success fails the reference",
     "base": "pcab_all_ok/context-single.log",
     "replace": [("alloc=0x00000001", "alloc=0x00000000")]},
    {"name": "gate/snippet Init_Ext not Success fails the reference",
     "base": "pcab_all_ok/context-single.log",
     "replace": [("snip_init=0x00000001", "snip_init=0x00000000")]},
    {"name": "gate/a ZERO feature handle fails the reference",
     "base": "pcab_all_ok/context-single.log",
     "replace": [("handle=0x1D4A2B0C000", "handle=0x0")]},
    {"name": "gate/a wrong blob size fails the reference",
     "base": "pcab_all_ok/context-single.log",
     "replace": [("blob=3944768", "blob=3944767")]},
    {"name": "gate/descriptor -1 fails the reference",
     "base": "pcab_all_ok/context-single.log",
     "replace": [("desc_status=0", "desc_status=-1")]},
]

#: THE FAIL-FAST CONTRACT (TASK 2), asserted on the exact set of processes
#: launched AND on the exact launcher invocations. SINGLE is a gate: when it does
#: not reproduce, the other three must never be started.
FAILFAST_CASES = [
    {"fixture": "pcab_all_ok",
     "launcher_calls": [["single"],
                        ["dual-held", "dual-active", "dual-released"]],
     "launched": ["single", "dual-held", "dual-active", "dual-released"],
     "archived": 4,
     "verdict": "MULTI_DEVICE_EXONERATED",
     "next": "STREAMLINE_CONTEXT_REQUIRED"},
    {"fixture": "pcab_single_fail",
     "launcher_calls": [["single"]],
     "launched": ["single"],
     "archived": 1,
     "verdict": "PROCESSCONTEXT_REFERENCE_INVALID",
     "next": None},
    {"fixture": "pcab_single_outofdate",
     "launcher_calls": [["single"]],
     "launched": ["single"],
     "archived": 1,
     "verdict": "PROCESSCONTEXT_REFERENCE_INVALID",
     "next": None},
    {"fixture": "pcab_held_fail",
     "launcher_calls": [["single"],
                        ["dual-held", "dual-active", "dual-released"]],
     "launched": ["single", "dual-held", "dual-active", "dual-released"],
     "archived": 4,
     "verdict": "SECOND_LIVE_D3D12_DEVICE_SUFFICIENT",
     "next": "SPLITPROCESS_ISOLATION"},
]


class _StubRunner(processcontext.ProcessContextRunner):
    """A ProcessContextRunner with the side-effecting seams replaced.

    `stage()` and `run_launcher()` are the ONLY places the real runner touches
    the filesystem and starts a process. Overriding exactly those two lets the
    fail-fast sequencing be tested for real - the launched-arm list below is
    produced by the real run() - without a GPU, without the artifact and without
    executing anything, including the launcher.
    """

    def __init__(self, cfg, log, fixture_dir):
        processcontext.ProcessContextRunner.__init__(self, cfg, log, gh=None)
        self.launched = []
        self.launcher_calls = []
        self.fixture_dir = fixture_dir

    def stage(self, artifact_meta, staging_dir):
        os.makedirs(staging_dir, exist_ok=True)
        return {"dir": staging_dir, "exe": "stub", "launcher": "stub",
                "nr_dll": "stub", "data_path": "stub"}

    def run_launcher(self, staged, arms):
        # The launcher is not executed. What is asserted is WHICH arms it was
        # asked for, and HOW MANY times - which is exactly what the gate decides.
        self.launched.extend(arms)
        self.launcher_calls.append(list(arms))
        for arm in arms:
            src = os.path.join(self.fixture_dir, "context-%s.log" % arm)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(staged["dir"], "context-%s.log" % arm))
        return {"arms": list(arms), "returncode": 0, "stdout": "", "stderr": "",
                "seconds": 0.0}

    def run_table(self, staged):
        return {"returncode": 4, "stdout": "(stub table)"}


class _StubSplitRunner(_StubRunner):
    """The split-process node with its side-effecting seams replaced.

    `stage`, `run_launcher`, `start_holder` and `stop_holder` are the only places
    run_split() touches the filesystem or starts a process, so overriding exactly
    those exercises the REAL sequencing - the two phases, the reference gate, the
    SHA256 comparison, and "PROCESS B is never launched without a live holder" -
    without a GPU, without the artifact and without executing anything.

    PHASE 1 and PROCESS B run the same arm and write the same log name, so the
    stub hands back a DIFFERENT fixture directory for the first and the second
    launcher call. That is what makes "the reference reproduced here" and "the
    lane failed in another process" separable in a test.
    """

    def __init__(self, cfg, log, phase1_dir, phase2_dir, holder_ok=True,
                 change_bytes=False):
        _StubRunner.__init__(self, cfg, log, phase2_dir)
        self.phase1_dir = phase1_dir
        self.phase2_dir = phase2_dir
        self.holder_ok = holder_ok
        self.change_bytes = change_bytes
        self.holder_started = False
        self.holder_stopped = False

    def stage(self, artifact_meta, staging_dir):
        os.makedirs(staging_dir, exist_ok=True)
        exe = os.path.join(staging_dir, "processcontext_ab.exe")
        holder = os.path.join(staging_dir, processcontext.HOLDER_EXE_NAME)
        # REAL files: run_split hashes the executable before each phase, and a
        # placeholder string would make that check untestable.
        with open(exe, "wb") as fh:
            fh.write(b"stub processcontext_ab.exe\n")
        with open(holder, "wb") as fh:
            fh.write(b"stub holder_ti.exe\n")
        return {"dir": staging_dir, "exe": exe, "holder_exe": holder,
                "launcher": "stub", "nr_dll": "stub", "data_path": "stub"}

    def run_launcher(self, staged, arms):
        self.launched.extend(arms)
        self.launcher_calls.append(list(arms))
        # The FIRST launcher call is PHASE 1; any later one is PROCESS B.
        source = self.phase1_dir if len(self.launcher_calls) == 1 else self.phase2_dir
        for arm in arms:
            src = os.path.join(source, "context-%s.log" % arm)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(staged["dir"], "context-%s.log" % arm))
        if len(self.launcher_calls) == 1 and self.change_bytes:
            # Simulate the executable being replaced between the two phases.
            with open(staged["exe"], "wb") as fh:
                fh.write(b"a DIFFERENT processcontext_ab.exe\n")
        return {"arms": list(arms), "returncode": 0, "stdout": "", "stderr": "",
                "seconds": 0.0}

    def start_holder(self, staged, event_name, on_line=None):
        self.holder_started = True
        # The holder writes its own log; produce one so the archive path is
        # exercised rather than skipped.
        held = os.path.join(staged["dir"], processcontext.HOLDER_LOG_NAME)
        with open(held, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("[holder] PROCESS A - active Ti SUPER D3D12 state (stub)\n")
            fh.write("HOLDER_READY\n" if self.holder_ok
                     else "HOLDER_FAILED reason=STUB_NO_ADAPTER\n")
        return {"process": None, "exe": staged["holder_exe"], "event_name": event_name,
                "ready": self.holder_ok, "failed": not self.holder_ok,
                "stopped": False,
                "lines": ["HOLDER_READY"] if self.holder_ok else ["HOLDER_FAILED"],
                "returncode": None, "alive_after_ready": self.holder_ok}

    def stop_holder(self, holder):
        self.holder_stopped = True
        holder["returncode"] = 0
        holder["stop_requested"] = True
        # A holder that never established its state never prints HOLDER_STOPPED
        # either: it exits on the failure path.
        holder["stopped"] = self.holder_ok
        return holder


def _stub_config():
    return {
        "repository": "stub/stub", "branch": "stub",
        "paths": {"cyberpunk_exe": "", "install_dir": "", "addon_name": "",
                  "reshade_log": "", "nr_dll": "",
                  "neuralscreen_nr_dll": "C:/stub/native/nvngx_dlssnr.dll"},
        "required_nr_dll_sha256": "0" * 64,
        "safety": {"allow_game_launch": False},
        "launch": {"exe": "", "args": [], "working_dir": ""},
        "timeouts": {"processcontext_seconds": 1, "game_run_seconds": 1,
                     "post_close_wait_seconds": 0, "gh_wait_seconds": 1,
                     "gh_poll_seconds": 1},
        "results_dir": "results", "cache_dir": "cache", "_cache_dir": "cache",
    }


def run_parser_tests(console):
    console("parser tests")
    failures = []
    passed = 0

    for test in PARSER_TESTS:
        path = os.path.join(FIXTURES, test["log"])
        try:
            with open(path, "r", encoding="utf-8") as fh:
                parser = log_parser.parse_reshade_log(fh.read())
            obs = parser.observation_summary()
        except OSError as exc:
            failures.append("%s: fixture unreadable (%s)" % (test["name"], exc))
            continue
        bad = []
        for key, want in test["expect"].items():
            got = obs.get(key)
            if got != want:
                bad.append("%s: expected %r, got %r" % (key, want, got))
        # The `[MGPU][P1.0c] CreateFeature(Reserved18) ...: result=0x...` line
        # must never be mistaken for a P1 Init result. A loose Init pattern does
        # exactly that, and this is the assertion that catches it.
        if test["log"] == "game_failing.log" and obs.get("p1_core_init") != 0x00000001:
            bad.append("p1_core_init was taken from the CreateFeature line, not the "
                       "Init line (got %r)" % (obs.get("p1_core_init"),))
        if bad:
            failures.append("%s:\n      %s" % (test["name"], "\n      ".join(bad)))
        else:
            passed += 1
            console("  PASS %s" % test["name"])

    # ---- negative controls ------------------------------------------------
    for ctrl in NEGATIVE_CONTROLS:
        path = os.path.join(FIXTURES, ctrl["base"])
        try:
            with open(path, "r", encoding="utf-8") as fh:
                text = fh.read()
        except OSError as exc:
            failures.append("%s: fixture unreadable (%s)" % (ctrl["name"], exc))
            continue
        for old, new in ctrl["replace"]:
            if old not in text:
                failures.append("%s: the fixture no longer contains %r - the control "
                                "is no longer mutating anything" % (ctrl["name"], old))
                text = None
                break
            text = text.replace(old, new)
        if text is None:
            continue
        parser = log_parser.parse_reshade_log(text)
        bad = []
        if "expect_gate_failed" in ctrl:
            game = experiments.build_game_result("APPID_NS", parser, {}, {})
            if game["valid"] is not False:
                bad.append("the validity gate still passed (valid=%r, reason=%r)"
                           % (game["valid"], game["reason"]))
        else:
            obs = parser.observation_summary()
            for key, want in ctrl["expect"].items():
                got = obs.get(key)
                if got != want:
                    bad.append("%s: expected %r, got %r" % (key, want, got))
        if bad:
            failures.append("%s:\n      %s" % (ctrl["name"], "\n      ".join(bad)))
        else:
            passed += 1
            console("  PASS %s" % ctrl["name"])

    # ---- the reference gate's eight conditions, each shown to matter ------
    #
    # Each control mutates a PASSING fixture and pushes it through the REAL gate
    # path - processcontext.parse_arm_logs, which evaluates the eight conditions
    # AND cross-checks the machine-readable line against the detailed [cuda]
    # lines. Testing gate_passes() alone would miss the case where a mutated
    # field is contradicted by the detailed line: the gate would pass on the
    # detailed value while the conflict check rejected the arm.
    for ctrl in REFERENCE_GATE_CONTROLS:
        path = os.path.join(FIXTURES, ctrl["base"].replace("/", os.sep))
        try:
            with open(path, "r", encoding="utf-8") as fh:
                text = fh.read()
        except OSError as exc:
            failures.append("%s: fixture unreadable (%s)" % (ctrl["name"], exc))
            continue
        mutated = text
        broke = False
        for old, new in ctrl["replace"]:
            if old not in mutated:
                failures.append("%s: the fixture no longer contains %r - the control is "
                                "no longer mutating anything" % (ctrl["name"], old))
                broke = True
                break
            mutated = mutated.replace(old, new)
        if broke:
            continue
        tmp = tempfile.mkdtemp(prefix="autolab-gate-")
        try:
            with open(os.path.join(tmp, "context-single.log"), "w",
                      encoding="utf-8", newline="\n") as fh:
                fh.write(mutated)
            arms = processcontext.parse_arm_logs(tmp, arms=["single"])
            summary = arms["single"]
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        if summary.get("reference_ok") is not False:
            failures.append(
                "%s: the reference gate STILL passed after mutating %r -> %r "
                "(reference_ok=%r, conflicts=%r)"
                % (ctrl["name"], ctrl["replace"][0][0], ctrl["replace"][0][1],
                   summary.get("reference_ok"), summary.get("parse_conflicts")))
        else:
            passed += 1
            console("  PASS %s" % ctrl["name"])

    # ---- TASK 2: SINGLE is a gate, not a datum ---------------------------
    # The real run() is exercised with only its two side-effecting seams
    # replaced, and the assertions are on the EXACT launcher invocations, the
    # exact arms launched, and the arm logs that were archived.
    node = experiments.node("PROCESSCONTEXT")
    for case in FAILFAST_CASES:
        fixture_dir = os.path.join(FIXTURES, case["fixture"])
        run_dir = tempfile.mkdtemp(prefix="autolab-failfast-")
        reporter = None
        try:
            reporter = report.Reporter(run_dir, console)
            runner = _StubRunner(_stub_config(), console, fixture_dir)
            result = runner.run({"artifact_dir": "(stub)"}, run_dir, reporter=reporter)
            # EXACTLY what autolab.run_processcontext_node does next.
            runner.archive_all_arm_logs({"dir": result["observations"]["staging_dir"]},
                                        reporter, result)
        except Exception as exc:                      # noqa: BLE001
            failures.append("failfast/%s: raised %s: %s"
                            % (case["fixture"], type(exc).__name__, exc))
            shutil.rmtree(run_dir, ignore_errors=True)
            continue
        decision = experiments.decide(node, result)
        bad = []
        if runner.launcher_calls != case["launcher_calls"]:
            bad.append("%s was launched as %r, expected %r"
                       % (processcontext.LAUNCHER_NAME, runner.launcher_calls,
                          case["launcher_calls"]))
        if runner.launched != case["launched"]:
            bad.append("launched %r, expected %r" % (runner.launched, case["launched"]))
        if decision.verdict != case["verdict"] or decision.next != case["next"]:
            bad.append("verdict %s/%s, expected %s/%s"
                       % (decision.verdict, decision.next, case["verdict"], case["next"]))
        archived = result["observations"].get("arm_logs_archived") or {}
        if len(archived) != case["archived"]:
            bad.append("archived %d arm log(s) (%s), expected %d"
                       % (len(archived), ", ".join(sorted(os.path.basename(p)
                                                          for p in archived.values())),
                          case["archived"]))
        logs_dir = os.path.join(run_dir, "logs")
        on_disk = sorted(os.listdir(logs_dir)) if os.path.isdir(logs_dir) else []
        if len(on_disk) != case["archived"]:
            bad.append("logs/ holds %r, expected %d file(s)"
                       % (on_disk, case["archived"]))
        if len(case["launched"]) == 1:
            # The point of the whole task: the other three were NOT started.
            if len(runner.launched) != 1:
                bad.append("SINGLE failed the reference and %d arm(s) were still launched"
                           % len(runner.launched))
            if result["observations"].get("arms_not_launched") != \
                    ["dual-held", "dual-active", "dual-released"]:
                bad.append("arms_not_launched was not recorded")
        reporter.close()
        shutil.rmtree(run_dir, ignore_errors=True)
        if bad:
            failures.append("failfast/%s:\n      %s" % (case["fixture"], "\n      ".join(bad)))
        else:
            passed += 1
            console("  PASS failfast/%s -> %d launcher call(s), launched %s, archived %d -> %s"
                    % (case["fixture"], len(runner.launcher_calls),
                       ",".join(runner.launched), len(archived), decision.verdict))

    # ---- SPLITPROCESS_ISOLATION: the two phases, in order, with PROCESS B
    #      launched only when the reference reproduced AND the holder is alive
    node = experiments.node("SPLITPROCESS_ISOLATION")
    for case in SPLIT_CASES:
        phase1_dir = os.path.join(FIXTURES, case["phase1_fixture"])
        fixture_dir = os.path.join(FIXTURES, case["fixture"])
        run_dir = tempfile.mkdtemp(prefix="autolab-split-")
        reporter = None
        label = "%s->%s holder_ok=%s" % (case["phase1_fixture"], case["fixture"],
                                         case["holder_ok"])
        try:
            reporter = report.Reporter(run_dir, console)
            runner = _StubSplitRunner(_stub_config(), console, phase1_dir, fixture_dir,
                                      holder_ok=case["holder_ok"],
                                      change_bytes=bool(case.get("change_bytes")))
            result = runner.run_split({"artifact_dir": "(stub)"}, run_dir,
                                      reporter=reporter)
            runner.archive_split_logs(result, reporter)
        except Exception as exc:                      # noqa: BLE001
            failures.append("split/%s: raised %s: %s"
                            % (label, type(exc).__name__, exc))
            shutil.rmtree(run_dir, ignore_errors=True)
            continue
        decision = experiments.decide(node, result)
        bad = []
        if runner.launcher_calls != case["launcher_calls"]:
            bad.append("launcher invocations %r, expected %r"
                       % (runner.launcher_calls, case["launcher_calls"]))
        if result.get("phase1_ok") is not case["phase1_ok"]:
            bad.append("phase1_ok=%r, expected %r"
                       % (result.get("phase1_ok"), case["phase1_ok"]))
        if runner.holder_started is not case["holder_started"]:
            bad.append("holder started=%r, expected %r"
                       % (runner.holder_started, case["holder_started"]))
        want_stopped = bool(case["holder_started"] and case["holder_ok"])
        if result.get("holder_stopped") is not want_stopped:
            bad.append("holder_stopped=%r, expected %r"
                       % (result.get("holder_stopped"), want_stopped))
        if result.get("exe_same_bytes") is bool(case.get("change_bytes")):
            bad.append("exe_same_bytes=%r, expected %r"
                       % (result.get("exe_same_bytes"), not case.get("change_bytes")))
        # PHASE 1 failing must never start the holder: the point of the phase is
        # to avoid measuring the environment and calling it isolation.
        if not case["phase1_ok"] and runner.holder_started:
            bad.append("the holder was started even though PHASE 1 failed")
        if case["phase1_ok"] and not runner.holder_started:
            bad.append("the holder was never started")
        if runner.holder_started and not runner.holder_stopped:
            bad.append("the holder was never stopped")
        # PROCESS B is the second launcher call and nothing else, and when it did
        # not run its summary must NOT be PHASE 1's numbers under another name.
        ran = len(runner.launcher_calls) == 2
        if result.get("process_b_ran") is not ran:
            bad.append("process_b_ran=%r, expected %r" % (result.get("process_b_ran"), ran))
        if not ran and result["arms"][experiments.PROCESSCONTEXT_REFERENCE_ARM] \
                .get("reference_ok") is not None:
            bad.append("PROCESS B did not run, yet arms.single carries a gate verdict")
        if decision.verdict != case["verdict"]:
            bad.append("verdict %s, expected %s" % (decision.verdict, case["verdict"]))
        if decision.next is not None or not decision.stop:
            bad.append("the split verdict must be terminal (next=%r stop=%r)"
                       % (decision.next, decision.stop))
        archived = (result["observations"].get("logs_archived") or {})
        if len(archived) != case["archived"]:
            bad.append("archived %r, expected %d file(s)"
                       % (sorted(os.path.basename(p) for p in archived.values()),
                          case["archived"]))
        names = [os.path.basename(p) for p in archived.values()]
        if "splitprocess-phase1.log" not in names:
            bad.append("the PHASE 1 reference log was not archived")
        if case["holder_started"] and "holder.log" not in names:
            bad.append("holder.log was not archived")
        if ran and "processcontext-single.log" not in names:
            bad.append("PROCESS B's log was not archived")

        # The SUMMARY TABLE and the EVIDENCE are assembled by build_rows() and
        # build_evidence(), which the dry run does not use - it builds its own
        # rows. Without this, a split result could decide correctly and still be
        # printed as an INVALID row, which is exactly what happened once before.
        row_names = [r["experiment"] for r in build_rows([result], [])]
        for want in ("SPLIT/PHASE1", "SPLIT/HOLDER", "SPLIT/SINGLE"):
            if want not in row_names:
                bad.append("the summary table has no %s row (got %r)" % (want, row_names))
        split_rows = {r["experiment"]: r for r in build_rows([result], [])}
        if split_rows["SPLIT/PHASE1"]["valid"] is not case["phase1_ok"]:
            bad.append("the SPLIT/PHASE1 row reports valid=%r, expected %r"
                       % (split_rows["SPLIT/PHASE1"]["valid"], case["phase1_ok"]))
        if split_rows["SPLIT/SINGLE"]["valid"] is not (
                result["arms"][experiments.PROCESSCONTEXT_REFERENCE_ARM].get("valid")):
            bad.append("the SPLIT/SINGLE row does not carry PROCESS B's validity")
        evidence = build_evidence([result], [])
        if not any("PHASE 1 reference-alone" in e for e in evidence):
            bad.append("the evidence does not report PHASE 1")
        if not any("PROCESS A" in e for e in evidence):
            bad.append("the evidence does not report PROCESS A")

        shutil.rmtree(run_dir, ignore_errors=True)
        if bad:
            failures.append("split/%s:\n      %s" % (label, "\n      ".join(bad)))
        else:
            passed += 1
            console("  PASS split/%s -> launcher %s, archived %d -> %s"
                    % (label, runner.launcher_calls or "(nothing)", len(archived),
                       decision.verdict))

    # The graph, over fixture logs, through the real parsing path.
    node = experiments.node("PROCESSCONTEXT")
    for fixture, want_verdict, want_next in GRAPH_SCENARIOS:
        log_dir = os.path.join(FIXTURES, fixture)
        try:
            arms = processcontext.parse_arm_logs(log_dir)
            result = processcontext.build_result(arms, {"fixture": fixture}, {}, {}, 0.0)
            decision = experiments.decide(node, result)
        except Exception as exc:                      # noqa: BLE001 - report, do not hide
            failures.append("graph/%s: raised %s: %s" % (fixture, type(exc).__name__, exc))
            continue
        if decision.verdict != want_verdict or decision.next != want_next:
            failures.append("graph/%s: expected %s/%s, got %s/%s"
                            % (fixture, want_verdict, want_next, decision.verdict,
                               decision.next))
        else:
            passed += 1
            console("  PASS graph/%s -> %s (next %s)"
                    % (fixture, decision.verdict, decision.next))

    # A rule must never be satisfied by an absent field.
    absent = {"arms": {"single": {"reference_ok": None}}}
    if experiments.evaluate({"field": "arms.single.reference_ok", "op": "eq",
                             "value": False}, absent):
        failures.append("predicate/is_false matched an unobserved field")
    else:
        passed += 1
        console("  PASS predicate/an unobserved field does not match eq")

    # Every graph rule points at a node that exists.
    problems = experiments.validate_graph()
    if problems:
        failures.extend("graph-structure: " + p for p in problems)
    else:
        passed += 1
        console("  PASS graph/structure")

    console("parser tests: %d passed, %d failed" % (passed, len(failures)))
    for line in failures:
        console("  FAIL %s" % line)
    return (not failures), passed, failures


# ===========================================================================
# Dry run
# ===========================================================================

def dry_run(cfg, console, reporter):
    """Exercise the parser and the controller on synthetic logs.

    Nothing is built, nothing is deployed and the game is never launched. The
    fixtures go through the SAME parse and decide functions the live run uses,
    so what this exercises is the real path and not a copy of it.
    """
    console("dry-run scenario")
    results = []
    rows = []
    node = experiments.node("PROCESSCONTEXT")

    # Imported historical results come first, exactly as in a live run, so the
    # dry run exercises the same summary shape.
    for imp in experiments.IMPORTED_RESULTS:
        rows.append({"experiment": imp["experiment"], "valid": imp["valid"],
                     "result": imp["verdict"], "note": imp.get("reason")})

    for fixture, want_verdict, want_next in GRAPH_SCENARIOS:
        console("experiment DRYRUN/%s" % fixture)
        log_dir = os.path.join(FIXTURES, fixture)
        arms = processcontext.parse_arm_logs(log_dir)
        result = processcontext.build_result(arms, {"fixture": fixture}, {}, {}, 0.0)
        decision = experiments.decide(node, result)
        # Name the scenario in the result itself, so the evidence block in the
        # summary says which fixture it came from rather than repeating
        # "PROCESSCONTEXT" six times.
        result["experiment"] = "DRYRUN/" + fixture
        result["verdict"] = decision.verdict
        result["next"] = decision.next
        results.append(result)

        single = arms.get(experiments.PROCESSCONTEXT_REFERENCE_ARM, {})
        console("P1 Init = %s" % _ngx_name(single.get("core_init")))
        console("Descriptor = %s" % _dec(single.get("descriptor_status")))
        console("CuModule = %s" % _dec(single.get("cumodule_status")))
        console("Reserved18 = %s" % _ngx_name(single.get("feature")))
        console("verdict %s" % decision.verdict)
        console("next %s" % (decision.next or "STOP"))

        ok = (decision.verdict == want_verdict and decision.next == want_next)
        reporter.event("dry_run_scenario", scenario=fixture, verdict=decision.verdict,
                       next=decision.next, expected=want_verdict, matched=ok)
        rows.append({"experiment": "DRYRUN/" + fixture.upper(),
                     "valid": ok, "result": decision.verdict})

    # The game path, from a fixture log, up to but not including the launch.
    console("experiment DRYRUN/APPID_NS (game path, no launch)")
    with open(os.path.join(FIXTURES, "game_failing.log"), "r", encoding="utf-8") as fh:
        parser = log_parser.parse_reshade_log(fh.read())
    live = LiveStatus(console, parser)
    live.update()
    live.finish()
    game = experiments.build_game_result("APPID_NS", parser, {"fixture": "game_failing.log"},
                                         {"tail_status": "fixture", "tail_seconds": 0.0})
    game["verdict"] = "FIXTURE_ONLY"
    results.append(game)
    rows.append({"experiment": "DRYRUN/APPID_NS-GAME-PATH",
                 "valid": game["valid"], "result": "FIXTURE_ONLY"})
    reporter.event("dry_run_scenario", scenario="APPID_NS-game-path",
                   valid=game["valid"], reason=game["reason"])

    # The split-process node, from the same fixtures. This is the node that
    # produces the SAME PROCESS vs SPLIT PROCESS comparison in the summary, so
    # the dry run exercises it too.
    split_node = experiments.node("SPLITPROCESS_ISOLATION")
    for case in SPLIT_CASES:
        tag = "%s-vs-%s-holder-%s%s" % (
            case["phase1_fixture"], case["fixture"],
            "ok" if case["holder_ok"] else "failed",
            "-bytes-changed" if case.get("change_bytes") else "")
        console("experiment DRYRUN/SPLIT-%s" % tag)
        phase1_dir = os.path.join(FIXTURES, case["phase1_fixture"])
        log_dir = os.path.join(FIXTURES, case["fixture"])
        run_dir = tempfile.mkdtemp(prefix="autolab-drysplit-")
        try:
            runner = _StubSplitRunner(_stub_config(), console, phase1_dir, log_dir,
                                      holder_ok=case["holder_ok"],
                                      change_bytes=bool(case.get("change_bytes")))
            split = runner.run_split({"artifact_dir": "(stub)"}, run_dir)
            runner.archive_split_logs(split, reporter)
        except Exception as exc:                      # noqa: BLE001
            console("  the split dry run raised %s: %s" % (type(exc).__name__, exc))
            shutil.rmtree(run_dir, ignore_errors=True)
            continue
        decision = experiments.decide(split_node, split)
        shutil.rmtree(run_dir, ignore_errors=True)
        split["experiment"] = "DRYRUN/SPLIT-" + tag
        split["verdict"] = decision.verdict
        split["next"] = decision.next
        results.append(split)
        shutil.rmtree(run_dir, ignore_errors=True)

        h = split["observations"]["holder"]
        p1 = (split["observations"].get("phase1") or {}).get("summary") or {}
        s = split["arms"].get(experiments.PROCESSCONTEXT_REFERENCE_ARM, {})
        console("PHASE 1 reference_ok = %s" % p1.get("reference_ok"))
        console("HOLDER_READY = %s" % ("yes" if h["ready"] else "no"))
        console("HOLDER_STOPPED = %s" % ("yes" if h["stopped_line"] else "no"))
        console("Descriptor = %s" % _dec(s.get("descriptor_status")))
        console("CuModule = %s" % _dec(s.get("cumodule_status")))
        console("Reserved18 = %s" % _ngx_name(s.get("feature")))
        console("verdict %s" % decision.verdict)
        console("next %s" % (decision.next or "STOP"))
        reporter.event("dry_run_scenario", scenario="split-" + tag,
                       verdict=decision.verdict, next=decision.next,
                       phase1_ok=split.get("phase1_ok"),
                       holder_ready=h["ready"],
                       process_b_launched=bool(split.get("process_b_ran")))
        rows.append({"experiment": "DRYRUN/SPLIT-" + tag.upper(),
                     "valid": split["valid"], "result": decision.verdict})

    classification = {
        "verdict": "DRY_RUN",
        "because": "synthetic fixture logs only; nothing was built, deployed or "
                   "launched",
        "next": None,
    }
    evidence = build_evidence(results, reporter._hashes)
    evidence.append("dry run: no build, no deploy, no game launch")
    return results, rows, classification, evidence


# ===========================================================================
# main
# ===========================================================================

def build_parser():
    ap = argparse.ArgumentParser(
        prog="autolab.py",
        description="MGPU-AutoLab - deterministic experiment orchestrator")
    ap.add_argument("--config", default=os.path.join(HERE, "config.yaml"))
    ap.add_argument("--results-dir", default=None)
    ap.add_argument("--only", default=None,
                    help="run a single named GRAPH NODE once and stop (maintenance)")
    ap.add_argument("--run-experiment", default=None,
                    help="run ONE named workflow-defined experiment outside the graph "
                         "and stop. A maintenance affordance: the graph is still the "
                         "only thing that decides what to try next, and the report "
                         "records this as a manual run")
    ap.add_argument("--no-cache", action="store_true",
                    help="force a fresh build instead of reusing the cache")
    ap.add_argument("--allow-game-launch", action="store_true",
                    help="permit the game launch this run (safety gate)")
    ap.add_argument("--dry-run", action="store_true",
                    help="synthetic fixtures only; no build, no deploy, no launch")
    ap.add_argument("--preflight-only", action="store_true")
    ap.add_argument("--parser-tests", action="store_true")
    ap.add_argument("--list", action="store_true", help="show the graph and exit")
    return ap


def cmd_list(console):
    console("graph: start = %s" % experiments.GRAPH["start"])
    for name in sorted(experiments.GRAPH["nodes"]):
        node = experiments.node(name)
        console("node %s (implemented=%s, workflow=%s)"
                % (name, node.get("implemented"), node.get("workflow", "-")))
        for rule in node.get("decisions", []):
            console("  rule %s -> %s (stop=%s, next=%s)"
                    % (rule["id"], rule["verdict"],
                       rule.get("stop", rule.get("next") is None), rule.get("next")))
            for cond in rule["when"]:
                console("       when %s %s%s"
                        % (cond["field"], cond["op"],
                           "" if "value" not in cond else " %r" % cond["value"]))
        fb = node["fallback"]
        console("  rule %s -> %s (stop=%s, next=%s)"
                % (fb["id"], fb["verdict"], fb.get("stop", fb.get("next") is None),
                   fb.get("next")))
        console("       when no other rule matches")
    for imp in experiments.IMPORTED_RESULTS:
        console("imported result: %s valid=%s verdict=%s"
                % (imp["experiment"], imp["valid"], imp["verdict"]))
    return 0


def main(argv=None):
    args = build_parser().parse_args(argv)
    console = Console()
    exit_code = 0

    try:
        cfg = load_config(args.config)
    except ConfigError as exc:
        sys.stderr.write("[AUTOLAB] config error: %s\n" % exc)
        return 2

    if args.allow_game_launch:
        cfg.setdefault("safety", {})["allow_game_launch"] = True

    if args.list:
        return cmd_list(console)

    if args.parser_tests:
        ok, passed, failures = run_parser_tests(console)
        if not ok:
            exit_code = 1
        return exit_code

    cfg["_cache_dir"] = resolve_dir(cfg, "cache_dir")
    results_root = args.results_dir or resolve_dir(cfg, "results_dir")

    problems = experiments.validate_graph()
    if problems:
        for p in problems:
            sys.stderr.write("[AUTOLAB] graph error: %s\n" % p)
        return 2

    # Validate the maintenance selectors BEFORE preflight, so a typo costs
    # nothing and touches nothing.
    if args.only and args.only not in experiments.GRAPH["nodes"]:
        sys.stderr.write(
            "[AUTOLAB] --only %r is not a graph node. The graph's nodes are: %s\n"
            "[AUTOLAB] (a workflow-defined experiment that is not a graph node is run "
            "with --run-experiment instead)\n"
            % (args.only, ", ".join(sorted(experiments.GRAPH["nodes"]))))
        return 2
    if args.run_experiment and args.run_experiment not in experiments.WORKFLOWS:
        sys.stderr.write(
            "[AUTOLAB] --run-experiment %r is not defined. Known experiments: %s\n"
            % (args.run_experiment, ", ".join(sorted(experiments.WORKFLOWS))))
        return 2
    if args.only and args.run_experiment:
        sys.stderr.write("[AUTOLAB] --only and --run-experiment are mutually exclusive\n")
        return 2

    stamp = time.strftime("%Y%m%d-%H%M%S")
    run_dir = os.path.join(results_root, stamp)
    reporter = report.Reporter(run_dir, console)

    deploy = deployment.Deployment(cfg, console)
    gh = github_actions.GithubActions(cfg, console)
    cp = cyberpunk.Cyberpunk(cfg, console)

    started = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    classification = {"verdict": "ABORTED", "because": "the run did not reach a verdict",
                      "next": None}
    results = []
    rows = []
    evidence = []
    run_meta = {"run_id": stamp, "started": started, "finished": "",
                "branch": cfg.get("branch"), "commit": "", "dry_run": bool(args.dry_run)}

    try:
        if args.dry_run:
            reporter.event("dry_run_start")
            results, rows, classification, evidence = dry_run(cfg, console, reporter)
            console("verdict %s" % classification["verdict"])
            console("next %s" % (classification["next"] or "STOP"))
        else:
            info = preflight(cfg, console, reporter, deploy, cp, gh, run_dir)
            run_meta["commit"] = gh.head_sha()
            reporter.event("preflight_ok", installed=info["installed_sha256"],
                           nr_dll=info["nr_dll_sha256"], commit=run_meta["commit"])

            if args.preflight_only:
                # Stop here. Preflight has taken and verified the backup; the
                # finally block restores, and with nothing deployed the restore
                # writes nothing. This path never touches the game directory.
                classification = {
                    "verdict": "PREFLIGHT_OK",
                    "because": "every preflight check passed; --preflight-only stops "
                               "before any build, deployment or launch",
                    "next": None,
                }
                console("verdict %s" % classification["verdict"])
                console("next STOP (--preflight-only)")
                reporter.event("preflight_only", verdict=classification["verdict"])
                rows = build_rows(results, experiments.IMPORTED_RESULTS)
                evidence = build_evidence(results, reporter._hashes)
                evidence.append("--preflight-only: no build, no deploy, no launch")
                return exit_code

            imported = experiments.IMPORTED_RESULTS

            # A manual run is explicit, named and outside the graph. It is
            # recorded as such so a report can never be mistaken for the graph
            # having chosen it.
            if args.run_experiment:
                name = args.run_experiment
                spec = experiments.WORKFLOWS[name]
                node = {"experiment": name, "implemented": True,
                        "workflow": name}
                console("manual run of %s (not a graph decision)" % name)
                reporter.event("manual_run", experiment=name)
                result, _meta = run_node(name, node, cfg, console, reporter, gh, deploy,
                                         cp, run_dir, use_cache=not args.no_cache)
                result["verdict"] = "MANUAL_RUN"
                result["next"] = None
                results.append(result)
                classification = {
                    "verdict": "MANUAL_RUN",
                    "because": "--run-experiment %s was requested explicitly; the graph "
                               "did not choose it and made no decision" % name,
                    "next": None,
                }
                console("verdict %s" % classification["verdict"])
                console("next STOP (manual run)")
                rows = build_rows(results, imported)
                evidence = build_evidence(results, reporter._hashes)
                evidence.append("manual run: %s was named on the command line" % name)
                return exit_code

            walk = [args.only] if args.only else [experiments.GRAPH["start"]]
            steps = 0
            while walk and steps < experiments.MAX_STEPS:
                steps += 1
                name = walk.pop(0)
                node = experiments.node(name)
                if not node.get("implemented"):
                    classification = {"verdict": node["verdict"],
                                      "because": node.get("because", ""),
                                      "next": None}
                    console("verdict %s" % classification["verdict"])
                    console("next STOP (%s is not implemented)" % name)
                    reporter.event("placeholder", node=name,
                                   verdict=classification["verdict"])
                    break
                result, _meta = run_node(name, node, cfg, console, reporter, gh, deploy,
                                         cp, run_dir, use_cache=not args.no_cache)
                decision = experiments.decide(node, result)
                result["verdict"] = decision.verdict
                result["next"] = decision.next
                results.append(result)
                classification = {"verdict": decision.verdict,
                                  "because": decision.because,
                                  "next": decision.next}
                console("verdict %s" % decision.verdict)
                console("next %s" % (decision.next or "STOP"))
                reporter.event("decision", experiment=name, rule=decision.id,
                               verdict=decision.verdict, next=decision.next,
                               stop=decision.stop, valid=result["valid"],
                               reason=result.get("reason"))
                if args.only:
                    break
                if decision.stop:
                    break
                if decision.next:
                    walk.append(decision.next)

            rows = build_rows(results, imported)
            evidence = build_evidence(results, reporter._hashes)

    except KeyboardInterrupt:
        console("interrupted")
        classification = {"verdict": "INTERRUPTED",
                          "because": "the user interrupted the run", "next": None}
        exit_code = 130
    except AutolabError as exc:
        console("ERROR %s: %s" % (type(exc).__name__, exc))
        classification = {"verdict": "ERROR", "because": str(exc), "next": None}
        exit_code = 1
    except Exception as exc:                          # noqa: BLE001 - must still restore
        console("UNEXPECTED ERROR %s: %s" % (type(exc).__name__, exc))
        classification = {"verdict": "ERROR", "because": "%s: %s" % (type(exc).__name__, exc),
                          "next": None}
        exit_code = 1
    finally:
        # RESTORE. Normal completion, Ctrl+C, an exception, a timeout and an
        # invalid result all arrive here.
        if deploy.has_backup():
            try:
                state = deploy.restore()
                if state.get("restored"):
                    console("deploy restore verified: %s" % state.get("sha256"))
                    reporter.hash("addon.restored", state.get("sha256") or "",
                                  path=state.get("path"))
                    reporter.event("restore", sha256=state.get("sha256"),
                                   already=state.get("already"))
            except Exception as exc:                  # noqa: BLE001
                console("RESTORE FAILED: %s" % exc)
                console("the original add-on is still at %s"
                        % (deploy.state()["backup"] or {}).get("path"))
                if exit_code == 0:
                    exit_code = 3
        run_meta["finished"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        try:
            reporter.write_environment(cfg, {
                "head commit": run_meta.get("commit") or "(not resolved)",
                "console": "%d [AUTOLAB] lines" % len(console.lines),
                "verdict": classification.get("verdict"),
            })
            if not rows:
                rows = build_rows(results, experiments.IMPORTED_RESULTS)
            if not evidence:
                evidence = build_evidence(results, reporter._hashes)
            summary_txt, summary_json = reporter.write_summary(
                rows, classification, evidence, run_meta)
            hashes_txt = reporter.write_hashes()
            console("results %s" % summary_txt)
            console("hashes  %s" % hashes_txt)
            console("summary %s" % summary_json)
            console("timeline %s" % os.path.join(run_dir, "timeline.jsonl"))
        except Exception as exc:                      # noqa: BLE001
            sys.stderr.write("[AUTOLAB] could not write the report: %s\n" % exc)
            if exit_code == 0:
                exit_code = 1
        reporter.close()

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
