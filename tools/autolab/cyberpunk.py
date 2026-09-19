"""MGPU-AutoLab - the Cyberpunk runner.

WHAT IT DOES, IN THE ORDER THE EXPERIMENT REQUIRES
    1. backup the installed add-on            (deployment.py, done in preflight)
    2. verify the downloaded artifact SHA     (deployment.py, done by the caller)
    3. deploy the exact artifact              (deployment.py, done by the caller)
    4. verify the installed SHA               (deployment.py, done by the caller)
    5. verify the NR DLL SHA                  (deployment.py, done in preflight)
    6. remove ONLY ReShade.log
    7. launch Cyberpunk normally
    8. tail ReShade.log continuously
    9. detect the required observations in real time

    ...then CloseMainWindow, wait, and only then force-terminate if it is still
    alive. The whole run is bounded by timeouts.game_run_seconds; a timeout is
    INVALID/TIMEOUT and never a guessed result.

WHAT IT DOES NOT DO
    It does not write anything into the game directory except ReShade.log (which
    it deletes) and the configured add-on (which deployment.py writes). It does
    not touch the NR runtime. It does not decide anything - it returns what it
    observed and lets the graph decide.

THE LAUNCH GATE
    launch() refuses unless cfg["safety"]["allow_game_launch"] is true. That is
    what lets AutoLab be developed and tested against synthetic fixtures without
    a game launch, and it is checked here rather than only in the CLI so no code
    path can bypass it.
"""

import csv
import io
import os
import subprocess
import time


class GameLaunchNotPermitted(Exception):
    pass


class CyberpunkError(Exception):
    pass


WM_CLOSE = 0x0010

try:                                    # Windows only; the module still imports elsewhere
    import ctypes
    from ctypes import wintypes

    _user32 = ctypes.WinDLL("user32", use_last_error=True)

    def _windows_for_pid(pid):
        found = []
        proto = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        def callback(hwnd, _lparam):
            owner = wintypes.DWORD()
            _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid:
                found.append(hwnd)
            return True

        _user32.EnumWindows(proto(callback), 0)
        return found
except Exception:                       # pragma: no cover - non-Windows
    _user32 = None

    def _windows_for_pid(pid):
        return []


def tasklist_pids(image_name):
    """PIDs of every process with this image name. [] when none."""
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq %s" % image_name, "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as exc:
        raise CyberpunkError("tasklist failed: %s" % (exc,))
    text = out.stdout or ""
    if "No tasks are running" in text or "INFO:" in text:
        return []
    pids = []
    for row in csv.reader(io.StringIO(text)):
        if len(row) >= 2 and row[0].strip().lower() == image_name.lower():
            try:
                pids.append(int(row[1]))
            except ValueError:
                pass
    return pids


class Cyberpunk(object):
    def __init__(self, cfg, log):
        self.exe = cfg["paths"]["cyberpunk_exe"]
        self.install_dir = cfg["paths"]["install_dir"]
        self.log_name = cfg["paths"]["reshade_log"]
        self.game_seconds = int(cfg["timeouts"]["game_run_seconds"])
        self.close_wait = int(cfg["timeouts"]["post_close_wait_seconds"])
        self.allow_launch = bool(cfg["safety"]["allow_game_launch"])
        launch = cfg.get("launch") or {}
        self.launch_exe = launch.get("exe") or self.exe
        self.launch_args = list(launch.get("args") or [])
        self.working_dir = launch.get("working_dir") or self.install_dir
        self.log = log
        self._proc = None
        self._image = os.path.basename(self.exe)

    # ---------------------------------------------------------------- paths
    @property
    def log_path(self):
        return os.path.join(self.install_dir, self.log_name)

    # ------------------------------------------------------------- process
    def running_pids(self):
        """Every PID of the game, whether or not AutoLab started it."""
        pids = set(tasklist_pids(self._image))
        if self._proc is not None and self._proc.poll() is None:
            pids.add(self._proc.pid)
        return sorted(pids)

    def is_running(self):
        return len(self.running_pids()) > 0

    def clear_log(self):
        """Remove ONLY ReShade.log, so the next run's log is entirely its own."""
        path = self.log_path
        if os.path.exists(path):
            try:
                os.remove(path)
                self.log("log cleared: %s" % path)
            except OSError as exc:
                raise CyberpunkError(
                    "could not remove %s (%s). Refusing to run: a stale log would be "
                    "parsed as this run's observations." % (path, exc))
        else:
            self.log("log absent before launch: %s" % path)

    def launch(self):
        if not self.allow_launch:
            raise GameLaunchNotPermitted(
                "game launch is disabled. Set safety.allow_game_launch: true in "
                "config.yaml (or pass --allow-game-launch) when you are ready for "
                "AutoLab to drive Cyberpunk.")
        if not os.path.isfile(self.launch_exe):
            raise CyberpunkError("the launch executable is not there: %s" % self.launch_exe)
        if self.is_running():
            raise CyberpunkError("Cyberpunk is already running (%s); refusing to launch a "
                                 "second copy." % self.running_pids())
        cmd = [self.launch_exe] + self.launch_args
        self.log("launching: %s (cwd %s)" % (" ".join(cmd), self.working_dir))
        self._proc = subprocess.Popen(cmd, cwd=self.working_dir, close_fds=True)
        return self._proc.pid

    def close_main_window(self):
        """Post WM_CLOSE to every top-level window the game owns.

        This is the graceful close, and it is tried first for every run.
        """
        if _user32 is None:
            return False
        posted = False
        for pid in self.running_pids():
            for hwnd in _windows_for_pid(pid):
                if _user32.PostMessageW(hwnd, WM_CLOSE, 0, 0):
                    posted = True
        if posted:
            self.log("CloseMainWindow posted (WM_CLOSE)")
        else:
            self.log("CloseMainWindow: no window to post to")
        return posted

    def terminate(self):
        """Last resort, after the graceful close and the wait have both failed."""
        pids = self.running_pids()
        for pid in pids:
            subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                           capture_output=True, text=True)
        if pids:
            self.log("force-terminated: %s" % (pids,))
        return pids

    # --------------------------------------------------------------- tail
    def tail(self, parser, complete, timeout_s=None, poll_s=0.25, on_line=None):
        """Tail ReShade.log, feeding every new line to `parser`.

        Stops as soon as `complete(parser)` is true - that is the point of
        tailing rather than sleeping for a fixed time: the run ends when the
        observations exist, not when a timer expires.

        Returns (status, detail) with status in {"complete", "timeout"}.
        """
        timeout_s = self.game_seconds if timeout_s is None else timeout_s
        path = self.log_path
        offset = 0
        started = time.time()
        saw_file = False

        while True:
            if os.path.isfile(path):
                saw_file = True
                try:
                    size = os.path.getsize(path)
                except OSError:
                    size = offset
                if size < offset:
                    # The log was truncated or replaced under us. The parser keeps
                    # what it already observed; only the read position resets.
                    self.log("log truncated under the tail; continuing from 0")
                    offset = 0
                if size > offset:
                    with open(path, "rb") as fh:
                        fh.seek(offset)
                        chunk = fh.read()
                        offset = fh.tell()
                    for raw in chunk.decode("utf-8", "replace").splitlines():
                        parser.feed(raw)
                        if on_line:
                            on_line(raw)
            if complete(parser):
                return ("complete", time.time() - started)
            if time.time() - started > timeout_s:
                return ("timeout", time.time() - started)
            if not saw_file and self._proc is not None and self._proc.poll() is not None:
                # The process exited without ever writing the log. Keep waiting
                # for the timeout so the caller reports TIMEOUT rather than a
                # result it does not have.
                pass
            time.sleep(poll_s)

    # ------------------------------------------------------------- archive
    def archive_log(self, dest_dir, label):
        """Copy the whole ReShade.log into the results tree before anything else
        can overwrite it."""
        os.makedirs(dest_dir, exist_ok=True)
        src = self.log_path
        dest = os.path.join(dest_dir, label)
        if not os.path.isfile(src):
            with open(dest + ".MISSING.txt", "w", encoding="utf-8") as fh:
                fh.write("no ReShade.log existed after this run: %s\n" % src)
            return None
        with open(src, "rb") as fin, open(dest, "wb") as fout:
            fout.write(fin.read())
        return dest
