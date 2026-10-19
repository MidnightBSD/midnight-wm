#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Open folders through the registered MIME handler in an isolated Xvfb session."""

import contextlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from settings_smoke import run, until, visible_window

REPO = Path(__file__).resolve().parents[1]
ENTRY_ID = "org.midnightbsd.Workspace.Files.desktop"
BUS_NAME = "org.midnightbsd.Workspace.FileManager"


def session(root):
    folder = root / "home" / "Folder with spaces"
    folder.mkdir()
    with (root / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(REPO / "build/midnight-wm")], stdout=log, stderr=subprocess.STDOUT)
        try:
            until(lambda: "true" in run("gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                  "--object-path", "/org/freedesktop/DBus", "--method",
                  "org.freedesktop.DBus.NameHasOwner", BUS_NAME), "Workspace folder service missing")
            assert ENTRY_ID in run("gio", "mime", "inode/directory"), "folder handler is not registered"
            run("gio", "open", str(folder))
            until(lambda: visible_window("Folder: " + str(folder)), "MIME handler did not open the folder")
            run(str(REPO / "build/midnightbsd-open-folder"), folder.as_uri())
            run(str(REPO / "build/midnightbsd-open-folder"))
            until(lambda: visible_window("Folder: " + str(root / "home")),
                  "no-argument launcher did not open Home")
            rejected = subprocess.run([str(REPO / "build/midnightbsd-open-folder"),
                                       "https://example.org/folder"], capture_output=True, timeout=10)
            assert rejected.returncode != 0, "remote URI was accepted"
            assert wm.poll() is None, "workspace exited while opening a folder"
            print("Registered folder MIME handler, launcher paths, and live workspace delivery passed")
        finally:
            wm.terminate()
            with contextlib.suppress(subprocess.TimeoutExpired):
                wm.wait(timeout=5)
            if wm.poll() is None:
                wm.kill()
                wm.wait(timeout=5)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--session":
        session(Path(sys.argv[2]))
        return
    for command in ("Xvfb", "dbus-run-session", "gdbus", "gio", "xdotool"):
        if not shutil.which(command):
            raise RuntimeError(f"required command not found: {command}")
    for binary in ("midnight-wm", "midnightbsd-open-folder"):
        if not (REPO / "build" / binary).is_file():
            raise RuntimeError(f"build {binary} first")
    with tempfile.TemporaryDirectory(prefix="midnight-folder-handler-") as temporary:
        root = Path(temporary)
        for relative in ("home", "config", "config-system", "data/applications", "runtime"):
            (root / relative).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        entry = (REPO / "data" / ENTRY_ID).read_text()
        entry = entry.replace("Exec=midnightbsd-open-folder %U",
                              f"Exec={REPO / 'build/midnightbsd-open-folder'} %U")
        entry = entry.replace("TryExec=midnightbsd-open-folder",
                              f"TryExec={REPO / 'build/midnightbsd-open-folder'}")
        (root / "data/applications" / ENTRY_ID).write_text(entry)
        (root / "data/applications/mimeinfo.cache").write_text(
            f"[MIME Cache]\ninode/directory={ENTRY_ID};\n")
        (root / "config/midnightworkspace-mimeapps.list").write_text(
            f"[Default Applications]\ninode/directory={ENTRY_ID};\n")
        (root / "config/mimeapps.list").write_text(
            f"[Added Associations]\ninode/directory={ENTRY_ID};\n")
        read_fd, write_fd = os.pipe()
        with (root / "xvfb.log").open("w") as log:
            xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                                     "1280x720x24", "-nolisten", "tcp"], pass_fds=(write_fd,),
                                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            os.close(write_fd)
            try:
                with os.fdopen(read_fd) as stream:
                    display = stream.readline().strip()
                if not display:
                    raise RuntimeError("Xvfb failed to start")
                environment = os.environ.copy()
                for variable in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID"):
                    environment.pop(variable, None)
                environment.update(DISPLAY=f":{display}", HOME=str(root / "home"),
                                   XDG_CURRENT_DESKTOP="MidnightWorkspace",
                                   XDG_CONFIG_HOME=str(root / "config"),
                                   XDG_CONFIG_DIRS=str(root / "config-system"),
                                   XDG_DATA_HOME=str(root / "data"),
                                   XDG_RUNTIME_DIR=str(root / "runtime"))
                process = subprocess.Popen(["dbus-run-session", "--", sys.executable,
                                            str(Path(__file__).resolve()), "--session", str(root)],
                                           env=environment, start_new_session=True)
                try:
                    if process.wait(timeout=40):
                        raise RuntimeError(f"folder handler smoke failed; logs in {root}")
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
