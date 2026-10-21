#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check optional XScreenSaver startup in an isolated X11 session."""

import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "build/midnight-wm").resolve()
    for program in ("Xvfb", "xprop", "dbus-run-session"):
        if not shutil.which(program):
            raise RuntimeError(f"Missing {program}")
    with tempfile.TemporaryDirectory(prefix="midnight-xscreensaver-") as temporary:
        root = Path(temporary)
        for name in ("bin", "home", "config/midnight-wm", "system", "data", "runtime"):
            (root / name).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        saver = root / "bin/xscreensaver"
        saver.write_text("#!/bin/sh\nprintf '%s\\n' \"$1\" >> \"$MIDNIGHT_SAVER_MARKER\"\n")
        saver.chmod(0o755)
        marker = root / "started"
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                                 "1280x720x24", "-nolisten", "tcp", "-noreset"], pass_fds=(write_fd,))
        os.close(write_fd)
        try:
            with os.fdopen(read_fd) as output:
                display = output.readline().strip()
            if not display:
                raise RuntimeError("Xvfb did not start")
            env = os.environ.copy()
            for name in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID"):
                env.pop(name, None)
            env.update(DISPLAY=f":{display}", HOME=str(root / "home"),
                       XDG_CONFIG_HOME=str(root / "config"), XDG_CONFIG_DIRS=str(root / "system"),
                       XDG_DATA_HOME=str(root / "data"), XDG_RUNTIME_DIR=str(root / "runtime"),
                       MIDNIGHT_SAVER_MARKER=str(marker),
                       PATH=str(root / "bin") + os.pathsep + str(binary.parent) + os.pathsep + env["PATH"])
            config = root / "config/midnight-wm/config.ini"

            def login(contents, expected):
                config.write_text(contents)
                marker.unlink(missing_ok=True)
                with (root / "wm.log").open("a") as log:
                    wm = subprocess.Popen(["dbus-run-session", "--", str(binary)], env=env,
                                          stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                    try:
                        deadline = time.monotonic() + 8
                        while time.monotonic() < deadline and not marker.exists():
                            if wm.poll() is not None:
                                raise RuntimeError("WM exited before XScreenSaver startup: " +
                                                   (root / "wm.log").read_text())
                            time.sleep(0.1)
                        if expected:
                            assert marker.read_text().splitlines() == ["--no-splash"], "XScreenSaver launch mismatch"
                        else:
                            assert not marker.exists(), "XScreenSaver started despite disabled setting or existing daemon"
                    finally:
                        os.killpg(wm.pid, signal.SIGTERM)
                        wm.wait(timeout=8)

            login("[Session]\nstart_xscreensaver=false\n", False)
            login("[Session]\n", True)
            subprocess.run(["xprop", "-root", "-f", "_SCREENSAVER_VERSION", "8s", "-set",
                            "_SCREENSAVER_VERSION", "existing"], env=env, check=True)
            assert "existing" in subprocess.check_output(
                ["xprop", "-root", "_SCREENSAVER_VERSION"], env=env, text=True)
            login("[Session]\nstart_xscreensaver=true\n", False)
            print("XScreenSaver disabled, default startup, and existing-daemon cases passed.")
        finally:
            xvfb.terminate()
            xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
