"""MGPU-AutoLab - deployment of the add-on under test, and its restoration.

THE ONE FILE AUTO-LAB WRITES IN THE GAME DIRECTORY
    paths.addon_name. Nothing else. The NR runtime is verified and NEVER
    replaced - it is the fixed instrument this whole investigation is measured
    against, and swapping it would silently change what every result means.

RESTORATION IS NOT OPTIONAL
    The original add-on is backed up, and its hash verified, BEFORE anything is
    written. restore() then puts those exact bytes back and verifies the
    installed file hash again. AutoLab calls it from a finally block, so a
    normal finish, a Ctrl+C, a Python exception, a timeout and an invalid result
    all restore. A run that leaves an experimental build installed silently is
    the one failure mode this module exists to prevent, so restoration also
    prints, and its result is recorded in hashes.txt.
"""

import os

from github_actions import sha256_bytes, sha256_file


class DeploymentError(Exception):
    pass


class Deployment(object):
    def __init__(self, cfg, log):
        paths = cfg["paths"]
        self.install_dir = paths["install_dir"]
        self.addon_name = paths["addon_name"]
        self.nr_dll_rel = paths["nr_dll"]
        self.ns_nr_dll = paths.get("neuralscreen_nr_dll") or ""
        self.required_nr_sha = cfg["required_nr_dll_sha256"].lower()
        self.log = log
        self._backup = None
        self._deployed = False

    # ---------------------------------------------------------------- paths
    @property
    def addon_path(self):
        return os.path.join(self.install_dir, self.addon_name)

    @property
    def nr_dll_path(self):
        return os.path.join(self.install_dir, self.nr_dll_rel.replace("/", os.sep))

    @property
    def ns_nr_dll_path(self):
        return self.ns_nr_dll.replace("/", os.sep) if self.ns_nr_dll else ""

    # ------------------------------------------------------------ integrity
    def _verify_one(self, path, what):
        if not path:
            raise DeploymentError("%s: no path is configured" % what)
        if not os.path.isfile(path):
            raise DeploymentError("%s is not at %s" % (what, path))
        actual = sha256_file(path)
        if actual.lower() != self.required_nr_sha:
            raise DeploymentError(
                "%s does not match the required NeuralScreen build.\n"
                "  path     : %s\n  actual   : %s\n  required : %s\n"
                "AutoLab does NOT replace the runtime. Put the required build in "
                "place yourself and re-run."
                % (what, path, actual.upper(), self.required_nr_sha.upper()))
        return actual

    def verify_nr_dll(self):
        """Hash the runtime the GAME loads. Raises on a mismatch. Never writes."""
        return self._verify_one(self.nr_dll_path, "the NR runtime in the game install")

    def verify_ns_nr_dll(self):
        """Hash the NEURALSCREEN REFERENCE runtime the PROCESSCONTEXT node uses.

        Located from configuration and verified here, before any build, so a
        missing or wrong reference runtime is refused before CI is asked to do
        anything. This is the copy the reference lane runs against.
        """
        return self._verify_one(self.ns_nr_dll_path,
                                "the NeuralScreen reference NR runtime")

    def addon_sha(self):
        return sha256_file(self.addon_path)

    # -------------------------------------------------------------- backup
    def backup(self, backup_dir):
        """Copy the installed add-on aside and verify the copy. Idempotent."""
        if self._backup is not None:
            return self._backup
        src = self.addon_path
        if not os.path.isfile(src):
            raise DeploymentError(
                "there is no installed add-on at %s to back up. AutoLab will not "
                "run an experiment it cannot undo." % src)
        data = _read_all(src)
        got = sha256_bytes(data)
        os.makedirs(backup_dir, exist_ok=True)
        dest = os.path.join(backup_dir, self.addon_name + ".original")
        with open(dest, "wb") as fh:
            fh.write(data)
        if sha256_file(dest) != got:
            raise DeploymentError("the backup at %s does not verify" % dest)
        self._backup = {"path": dest, "sha256": got, "size_bytes": len(data),
                        "restored": False}
        self.log("deploy backup: %s (%s)" % (dest, got))
        return self._backup

    def has_backup(self):
        return self._backup is not None

    # -------------------------------------------------------------- deploy
    def deploy(self, source_path):
        """Write `source_path` to the add-on target and verify what landed."""
        if self._backup is None:
            raise DeploymentError("deploy() without a backup - refusing")
        if not os.path.isfile(source_path):
            raise DeploymentError("no artifact to deploy at %s" % source_path)
        data = _read_all(source_path)
        expected = sha256_bytes(data)

        target = self.addon_path
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())

        installed = sha256_file(target)
        if installed != expected:
            # Put the original back immediately rather than leaving a partial
            # write in place for the caller to notice.
            self.restore(force=True)
            raise DeploymentError(
                "the deployed add-on does not verify: expected %s, installed %s. "
                "The original add-on has been restored." % (expected, installed))
        self._deployed = True
        self.log("deploy installed: %s (%s)" % (target, installed))
        return {"path": target, "sha256": installed, "size_bytes": len(data),
                "source": source_path}

    # ------------------------------------------------------------- restore
    def restore(self, force=False):
        """Put the original add-on back and verify it. Safe to call twice."""
        if self._backup is None:
            return {"restored": False, "reason": "no backup was taken"}
        if self._backup.get("restored") and not force:
            return {"restored": True, "path": self.addon_path,
                    "sha256": self._backup["sha256"], "already": True, "wrote": False}

        target = self.addon_path
        if not force and os.path.isfile(target):
            # Nothing to undo: the installed add-on is already the backed-up
            # bytes. Writing them again would be a pointless write into the game
            # directory, and a pointless write is a pointless risk. This is what
            # makes --preflight-only completely read-only as far as the game
            # directory is concerned.
            current = sha256_file(target)
            if current == self._backup["sha256"]:
                self._backup["restored"] = True
                self._deployed = False
                self.log("deploy restore: the installed add-on already matches the backup "
                         "(%s); nothing written" % current)
                return {"restored": True, "path": target, "sha256": current,
                        "already": True, "wrote": False}

        data = _read_all(self._backup["path"])
        expected = sha256_bytes(data)
        if expected != self._backup["sha256"]:
            raise DeploymentError(
                "the BACKUP itself no longer verifies (%s vs recorded %s). AutoLab "
                "will not write it over the installed add-on. The backup is at %s"
                % (expected, self._backup["sha256"], self._backup["path"]))
        with open(target, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        installed = sha256_file(target)
        if installed != expected:
            raise DeploymentError(
                "restoration FAILED to verify: expected %s, installed %s. The "
                "original is still at %s - copy it over %s by hand."
                % (expected, installed, self._backup["path"], target))
        self._backup["restored"] = True
        self._deployed = False
        self.log("deploy RESTORED the original add-on: %s (%s)" % (target, installed))
        return {"restored": True, "path": target, "sha256": installed,
                "already": False, "wrote": True}

    def state(self):
        return {"backup": self._backup, "deployed_since_backup": self._deployed,
                "addon_path": self.addon_path, "nr_dll_path": self.nr_dll_path}


def _read_all(path):
    with open(path, "rb") as fh:
        return fh.read()
