"""MGPU-AutoLab - the results tree.

    results/<timestamp>/
        summary.txt        the table, the FINAL CLASSIFICATION and the EVIDENCE
        summary.json       the same thing, machine-readable, complete
        timeline.jsonl     one JSON object per event, in order, appended live
        hashes.txt         every hash the run saw, and what it belonged to
        environment.txt    machine, interpreter, repository state, config
        logs/              the archived ReShade.log and the four arm logs

WHAT summary.txt MAY NOT CONTAIN
    Speculative wording in the classification. The RESULT and CLASSIFICATION
    strings are drawn from fixed vocabularies defined here and in the graph -
    "EXONERATED", "REFERENCE_OK", "SECOND_LIVE_D3D12_DEVICE_SUFFICIENT" and so
    on. Nothing in this module composes a sentence that could be read as a new
    claim; it prints what was observed and which predefined verdict applied.
"""

import json
import os
import platform
import shutil
import sys
import time

RESULT_REFERENCE_OK = "REFERENCE_OK"
RESULT_REFERENCE_FAILED = "REFERENCE_FAILED"
RESULT_NOT_RUN = "NOT_RUN"
RESULT_INVALID = "INVALID"

W_EXPERIMENT = 30
W_VALID = 10


def _cell(valid):
    if valid is True:
        return "YES"
    if valid is False:
        return "NO"
    return "UNKNOWN"


class Reporter(object):
    def __init__(self, run_dir, log):
        self.run_dir = run_dir
        self.logs_dir = os.path.join(run_dir, "logs")
        os.makedirs(self.logs_dir, exist_ok=True)
        self.log = log
        self._seq = 0
        self._timeline = open(os.path.join(run_dir, "timeline.jsonl"), "a",
                              encoding="utf-8")
        self._hashes = []
        self._events = []

    # ------------------------------------------------------------- timeline
    def event(self, kind, **fields):
        self._seq += 1
        rec = {"seq": self._seq, "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "event": kind}
        rec.update(fields)
        self._timeline.write(json.dumps(rec, sort_keys=True) + "\n")
        self._timeline.flush()
        self._events.append(rec)
        return rec

    def close(self):
        try:
            self._timeline.close()
        except Exception:
            pass

    # ---------------------------------------------------------------- hashes
    def hash(self, label, value, path=None, expected=None):
        rec = {"label": label, "sha256": (value or "").lower()}
        if path:
            rec["path"] = path
        if expected:
            rec["expected"] = expected.lower()
            rec["match"] = rec["sha256"] == rec["expected"]
        self._hashes.append(rec)
        return rec

    def write_hashes(self):
        lines = ["# every hash this run observed, in order", ""]
        for rec in self._hashes:
            lines.append("%s  %s" % (rec["sha256"], rec["label"]))
            if "path" in rec:
                lines.append("    path     : %s" % rec["path"])
            if "expected" in rec:
                lines.append("    expected : %s" % rec["expected"])
                lines.append("    match    : %s" % ("YES" if rec.get("match") else "NO"))
        path = os.path.join(self.run_dir, "hashes.txt")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        return path

    # ----------------------------------------------------------- environment
    def write_environment(self, cfg, extra=None):
        lines = []
        lines.append("MGPU-AutoLab environment")
        lines.append("=" * 72)
        lines.append("timestamp        : %s" % time.strftime("%Y-%m-%dT%H:%M:%S%z"))
        lines.append("python           : %s" % sys.version.replace("\n", " "))
        lines.append("executable       : %s" % sys.executable)
        lines.append("platform         : %s" % platform.platform())
        lines.append("machine          : %s" % platform.machine())
        lines.append("node             : %s" % platform.node())
        lines.append("cwd              : %s" % os.getcwd())
        lines.append("")
        lines.append("repository       : %s" % cfg.get("repository"))
        lines.append("branch           : %s" % cfg.get("branch"))
        for key, value in sorted((extra or {}).items()):
            lines.append("%-17s: %s" % (key, value))
        lines.append("")
        lines.append("configured paths and gates")
        lines.append("-" * 72)
        for group in ("paths", "safety", "launch", "timeouts"):
            block = cfg.get(group) or {}
            for key in sorted(block):
                lines.append("%s.%s = %s" % (group, key, block[key]))
        lines.append("")
        lines.append("NOTE: no token, credential or secret is written here. The")
        lines.append("presence of a token source is reported, never its value.")
        path = os.path.join(self.run_dir, "environment.txt")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        return path

    # ---------------------------------------------------------------- logs
    def archive(self, src_path, label):
        if not src_path or not os.path.isfile(src_path):
            return None
        dest = os.path.join(self.logs_dir, label)
        shutil.copy2(src_path, dest)
        return dest

    # -------------------------------------------------------------- summary
    def write_summary(self, rows, classification, evidence, run_meta):
        """rows: list of dicts {experiment, valid, result, note}"""
        out = []
        out.append("=" * 78)
        out.append("MGPU-AutoLab - run summary")
        out.append("=" * 78)
        out.append("run              : %s" % run_meta.get("run_id", ""))
        out.append("started          : %s" % run_meta.get("started", ""))
        out.append("finished         : %s" % run_meta.get("finished", ""))
        out.append("commit           : %s" % run_meta.get("commit", ""))
        out.append("branch           : %s" % run_meta.get("branch", ""))
        out.append("dry run          : %s" % ("YES" if run_meta.get("dry_run") else "NO"))
        out.append("")
        out.append("%-*s%-*s%s" % (W_EXPERIMENT, "EXPERIMENT", W_VALID, "VALID", "RESULT"))
        out.append("-" * 78)
        for row in rows:
            out.append("%-*s%-*s%s" % (W_EXPERIMENT, row["experiment"], W_VALID,
                                       _cell(row.get("valid")), row["result"]))
            if row.get("note"):
                out.append("%-*s%s" % (W_EXPERIMENT, "", row["note"]))
        out.append("")
        out.append("-" * 78)
        out.append("FINAL CLASSIFICATION:")
        out.append("    %s" % classification.get("verdict", "NONE"))
        if classification.get("because"):
            out.append("    %s" % classification["because"])
        if classification.get("next"):
            out.append("    NEXT: %s" % classification["next"])
        out.append("")
        out.append("EVIDENCE:")
        for line in evidence:
            out.append("    %s" % line)
        out.append("")
        out.append("Results directory: %s" % self.run_dir)
        text = "\n".join(out) + "\n"

        summary_txt = os.path.join(self.run_dir, "summary.txt")
        with open(summary_txt, "w", encoding="utf-8") as fh:
            fh.write(text)

        summary_json = os.path.join(self.run_dir, "summary.json")
        with open(summary_json, "w", encoding="utf-8") as fh:
            json.dump({"run": run_meta, "rows": rows,
                       "classification": classification, "evidence": evidence,
                       "events": self._events, "hashes": self._hashes},
                      fh, indent=2, sort_keys=True)
        return summary_txt, summary_json
