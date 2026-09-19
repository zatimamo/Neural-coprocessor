"""MGPU-AutoLab - GitHub Actions: trigger, wait, download, hash, cache.

THE ONE RULE THIS MODULE ENFORCES
    An artifact is identified by the EXACT run that produced it. AutoLab
    dispatches the workflow itself, records the run id, waits for that run, and
    downloads the artifact attached to that run id. "The latest artifact" is
    never accepted, and an artifact is never reused without the commit SHA and
    the run id that produced it being recorded alongside it.

HOW IT TALKS TO GITHUB
    The REST API, through urllib from the standard library. `gh` is used when it
    is installed, but only as a TOKEN SOURCE and for `gh auth status` - there is
    one code path that performs the API calls, not two, so the two backends
    cannot disagree about which run was triggered. When `gh` is absent the token
    comes from the environment (GH_TOKEN / GITHUB_TOKEN) or from the same
    credential helper git already uses for this repository, which is how the
    dispatch was verified to work on this machine.

    A 204 from POST /dispatches means "accepted", not "started", so the run is
    then located by matching the branch head SHA and the trigger time. A run
    that cannot be located is an error, not a fallback to the newest run.
"""

import hashlib
import io
import json
import os
import shutil
import subprocess
import time
import urllib.error
import urllib.request
import zipfile

API = "https://api.github.com"


class GithubError(Exception):
    pass


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


class _StripAuthOnRedirect(urllib.request.HTTPRedirectHandler):
    """Artifact downloads redirect to signed storage.

    The signed URL carries its own authorization in the query string, and
    forwarding the API bearer token to a different host is at best pointless and
    at worst a 400. It is dropped on any redirect that leaves the API host.
    """

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and not newurl.startswith(API):
            new.headers.pop("Authorization", None)
            new.headers.pop("authorization", None)
        return new


class GithubActions(object):
    def __init__(self, cfg, log):
        self.repo = cfg["repository"]
        self.branch = cfg["branch"]
        self.timeout = int(cfg["timeouts"]["gh_wait_seconds"])
        self.poll = int(cfg["timeouts"]["gh_poll_seconds"])
        self.cache_dir = cfg["_cache_dir"]
        self.log = log
        self._gh = shutil.which("gh")
        self._token = None
        self._opener = urllib.request.build_opener(_StripAuthOnRedirect)

    # ---------------------------------------------------------------- auth
    def _token_from_gh(self):
        if not self._gh:
            return None
        try:
            out = subprocess.run([self._gh, "auth", "token"], capture_output=True,
                                 text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            return None
        if out.returncode != 0:
            return None
        tok = (out.stdout or "").strip()
        return tok or None

    def _token_from_git_credential(self):
        try:
            out = subprocess.run(
                ["git", "credential", "fill"],
                input="protocol=https\nhost=github.com\n\n",
                capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            return None
        if out.returncode != 0:
            return None
        for line in (out.stdout or "").splitlines():
            if line.startswith("password="):
                return line.split("=", 1)[1].strip() or None
        return None

    def token(self):
        if self._token:
            return self._token
        for env in ("GH_TOKEN", "GITHUB_TOKEN"):
            if os.environ.get(env):
                self._token = os.environ[env].strip()
                return self._token
        tok = self._token_from_gh()
        if tok:
            self._token = tok
            return self._token
        tok = self._token_from_git_credential()
        if tok:
            self._token = tok
            return self._token
        raise GithubError(
            "no GitHub token found. Set GH_TOKEN or GITHUB_TOKEN, install and "
            "authenticate gh, or make `git credential fill` for host github.com "
            "return a token.")

    def auth_check(self):
        """Preflight. Returns a human-readable description of what was found."""
        if self._gh:
            try:
                out = subprocess.run([self._gh, "auth", "status"], capture_output=True,
                                     text=True, timeout=30)
                if out.returncode != 0:
                    raise GithubError("`gh auth status` failed:\n%s"
                                      % ((out.stderr or out.stdout or "").strip(),))
                source = "gh auth status (exit 0)"
            except (OSError, subprocess.SubprocessError) as exc:
                raise GithubError("could not run gh: %s" % (exc,))
        else:
            source = "gh is not installed; using the REST API"
        self.token()                      # raises if there is no usable token
        data = self._get("/repos/%s" % self.repo)
        return "%s; repository %s reachable (default branch %s)" % (
            source, self.repo, data.get("default_branch"))

    # ------------------------------------------------------------- REST core
    def _request(self, method, path, body=None, raw=False):
        url = path if path.startswith("http") else API + path
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + self.token())
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("User-Agent", "MGPU-AutoLab")
        req.add_header("X-GitHub-Api-Version", "2022-11-28")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with self._opener.open(req, timeout=120) as resp:
                payload = resp.read()
        except urllib.error.HTTPError as exc:
            detail = ""
            try:
                detail = exc.read().decode("utf-8", "replace")[:400]
            except Exception:
                pass
            raise GithubError("%s %s -> HTTP %s %s" % (method, url, exc.code, detail))
        except urllib.error.URLError as exc:
            raise GithubError("%s %s -> %s" % (method, url, exc.reason))
        if raw:
            return payload
        if not payload:
            return None
        try:
            return json.loads(payload.decode("utf-8"))
        except ValueError:
            return None

    def _get(self, path):
        return self._request("GET", path)

    # ------------------------------------------------------------- branch
    def head_sha(self):
        data = self._get("/repos/%s/branches/%s" % (self.repo, self.branch))
        sha = data["commit"]["sha"]
        return sha

    # ------------------------------------------------------------ trigger
    def trigger(self, workflow_file):
        """Dispatch the workflow on the configured branch. Returns nothing.

        A 204 means the dispatch was accepted. It does NOT return the run id, so
        the run is located by wait_for_run() from the trigger time and the head
        SHA.
        """
        path = "/repos/%s/actions/workflows/%s/dispatches" % (self.repo, workflow_file)
        before = int(time.time()) - 5
        self._request("POST", path, body={"ref": self.branch})
        return before

    def wait_for_run(self, workflow_file, head_sha, not_before, timeout_s=120):
        """Find the run this dispatch created: same workflow, same SHA, new."""
        deadline = time.time() + timeout_s
        path = ("/repos/%s/actions/workflows/%s/runs?branch=%s&event=workflow_dispatch"
                "&per_page=20" % (self.repo, workflow_file, self.branch))
        while time.time() < deadline:
            data = self._get(path)
            for run in (data or {}).get("workflow_runs", []):
                created = run.get("created_at", "")
                if run.get("head_sha") != head_sha:
                    continue
                # created_at is ISO-8601 Z; compare as text after normalising.
                if created and created < _iso(not_before):
                    continue
                return run
            time.sleep(3)
        raise GithubError(
            "dispatched %s on %s but no new run for head %s appeared within %ss"
            % (workflow_file, self.branch, head_sha[:7], timeout_s))

    def wait_completion(self, run_id):
        """Block until the run completes. Returns the run. Raises unless success."""
        deadline = time.time() + self.timeout
        path = "/repos/%s/actions/runs/%s" % (self.repo, run_id)
        last = None
        while time.time() < deadline:
            run = self._get(path)
            status = run.get("status")
            if status != last:
                self.log("build run %s: %s" % (run_id, status))
                last = status
            if status == "completed":
                if run.get("conclusion") != "success":
                    raise GithubError("run %s concluded %r - refusing to use its artifact"
                                      % (run_id, run.get("conclusion")))
                return run
            time.sleep(self.poll)
        raise GithubError("run %s did not complete within %ss" % (run_id, self.timeout))

    # ----------------------------------------------------------- artifacts
    def find_artifact(self, run_id, name):
        data = self._get("/repos/%s/actions/runs/%s/artifacts" % (self.repo, run_id))
        for art in (data or {}).get("artifacts", []):
            if art.get("name") == name:
                return art
        names = [a.get("name") for a in (data or {}).get("artifacts", [])]
        raise GithubError("run %s has no artifact named %r (found: %s)"
                          % (run_id, name, ", ".join(names) or "none"))

    def download_artifact(self, run_id, name, dest_dir):
        art = self.find_artifact(run_id, name)
        blob = self._request(
            "GET", "/repos/%s/actions/artifacts/%s/zip" % (self.repo, art["id"]), raw=True)
        if not blob[:2] == b"PK":
            raise GithubError("artifact %s (id %s) did not download as a zip (%d bytes)"
                              % (name, art["id"], len(blob)))
        if os.path.isdir(dest_dir):
            shutil.rmtree(dest_dir)
        os.makedirs(dest_dir, exist_ok=True)
        with zipfile.ZipFile(io.BytesIO(blob)) as zf:
            for member in zf.namelist():
                # Refuse absolute or parent-traversing members outright.
                norm = member.replace("\\", "/")
                if norm.startswith("/") or ".." in norm.split("/"):
                    raise GithubError("artifact %s contains an unsafe path %r"
                                      % (name, member))
            zf.extractall(dest_dir)
        return {
            "artifact_id": art["id"],
            "artifact_name": art["name"],
            "artifact_zip_sha256": sha256_bytes(blob),
            "artifact_size_bytes": len(blob),
            "extracted_to": dest_dir,
        }

    # --------------------------------------------------------------- cache
    def cache_path(self, sha, experiment):
        return os.path.join(self.cache_dir, sha, experiment)

    def cached(self, sha, experiment):
        """Return the recorded artifact metadata if this SHA+experiment is cached.

        The cache is keyed by commit SHA and experiment name. A cached entry is
        reused only when its recorded SHA matches, so a cache cannot silently
        serve an artifact built from a different revision.
        """
        base = self.cache_path(sha, experiment)
        meta_path = os.path.join(base, "meta.json")
        if not os.path.isfile(meta_path):
            return None
        try:
            with open(meta_path, "r", encoding="utf-8") as fh:
                meta = json.load(fh)
        except (OSError, ValueError):
            return None
        if meta.get("commit") != sha:
            return None
        addon = meta.get("addon_path")
        if not addon or not os.path.isfile(addon):
            return None
        if sha256_file(addon) != meta.get("addon_sha256"):
            # The cached bytes changed under the recorded hash. Do not use them.
            return None
        meta["from_cache"] = True
        return meta

    def store(self, sha, experiment, meta):
        base = self.cache_path(sha, experiment)
        os.makedirs(base, exist_ok=True)
        meta = dict(meta)
        meta["from_cache"] = False
        with open(os.path.join(base, "meta.json"), "w", encoding="utf-8") as fh:
            json.dump(meta, fh, indent=2, sort_keys=True)
        return meta

    # ------------------------------------------------------------ the flow
    def build(self, experiment, workflow_file, artifact_name, addon_relpath,
              reuse_cache=True):
        """Build one experiment's artifact and return its metadata.

        Steps, in order: cache lookup by commit SHA + experiment; otherwise
        dispatch, locate the run, wait for success, download the exact artifact
        of that run, and hash the add-on inside it.
        """
        sha = self.head_sha()
        if reuse_cache:
            hit = self.cached(sha, experiment)
            if hit:
                self.log("build %s: cache hit for %s (run %s)"
                         % (experiment, sha[:7], hit.get("run_id")))
                return hit

        self.log("build %s: dispatching %s on %s at %s"
                 % (experiment, workflow_file, self.branch, sha[:7]))
        not_before = self.trigger(workflow_file)
        run = self.wait_for_run(workflow_file, sha, not_before)
        run_id = run["id"]
        self.log("build %s: run %s (%s)" % (experiment, run_id, run.get("html_url", "")))
        self.wait_completion(run_id)

        dest = os.path.join(self.cache_path(sha, experiment), "artifact")
        info = self.download_artifact(run_id, artifact_name, dest)
        addon = os.path.join(dest, addon_relpath)
        if not os.path.isfile(addon):
            raise GithubError(
                "artifact %s from run %s does not contain %s (it holds: %s)"
                % (artifact_name, run_id, addon_relpath,
                   ", ".join(sorted(os.path.relpath(os.path.join(r, f), dest)
                                    for r, _d, fs in os.walk(dest) for f in fs))))
        meta = {
            "experiment": experiment,
            "commit": sha,
            "workflow": workflow_file,
            "run_id": run_id,
            "run_url": run.get("html_url"),
            "run_attempt": run.get("run_attempt"),
            "artifact_id": info["artifact_id"],
            "artifact_name": info["artifact_name"],
            "artifact_zip_sha256": info["artifact_zip_sha256"],
            "artifact_size_bytes": info["artifact_size_bytes"],
            "artifact_dir": dest,
            "addon_path": addon,
            "addon_sha256": sha256_file(addon),
            "built_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        self.store(sha, experiment, meta)
        self.log("build %s: addon sha256 %s" % (experiment, meta["addon_sha256"]))
        return meta


def _iso(epoch_seconds):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch_seconds))
