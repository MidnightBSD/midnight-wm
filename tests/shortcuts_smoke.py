#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check live shortcut regrabs in a disposable Xvfb session."""

import contextlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.check_output(args, text=True, timeout=10).strip()


def until(predicate, message, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError(message)


def session(config):
    binary = ROOT / "build/midnight-wm"
    wm = subprocess.Popen([str(binary)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        until(lambda: "_NET_DESKTOP_NAMES" in run("xprop", "-root", "_NET_DESKTOP_NAMES"),
              "WM did not initialize")
        time.sleep(3)
        def desktop():
            return run("xdotool", "get_desktop")

        def press(chord):
            run("xdotool", "key", chord)

        press("ctrl+alt+Right")
        until(lambda: desktop() == "1", "default workspace shortcut failed")
        run("xdotool", "set_desktop", "0")
        until(lambda: desktop() == "0", "could not reset workspace")

        def save(value):
            temporary = config.with_suffix(".new")
            temporary.write_text("[Session]\nstart_xscreensaver=false\n[Shortcuts]\nworkspace_right=" + value + "\n")
            temporary.replace(config)
            time.sleep(2.5)  # WM polls once per second and waits for a stable snapshot.

        save("Super+F8")
        press("ctrl+alt+Right")
        time.sleep(0.3)
        assert desktop() == "0", "old shortcut remained grabbed after live update"
        press("super+F8")
        until(lambda: desktop() == "1", "new shortcut did not switch workspaces")
        run("xdotool", "set_desktop", "0")
        until(lambda: desktop() == "0", "could not reset workspace")

        save("None")
        press("super+F8")
        time.sleep(0.3)
        assert desktop() == "0", "disabled shortcut still switched workspaces"

        save("Alt+Tab")  # Conflicts with Switch window: retain the default set.
        press("ctrl+alt+Right")
        until(lambda: desktop() == "1", "conflicting config replaced default shortcuts")
        assert wm.poll() is None, "WM exited during shortcut updates"
        second = subprocess.run([str(binary)], capture_output=True, timeout=10)
        assert second.returncode != 0, "second WM took over the test display"
        print("Shortcut default, live regrab, disable, conflict fallback, and second-WM checks passed.")
    finally:
        wm.terminate()
        try:
            wm.wait(timeout=6)
        except subprocess.TimeoutExpired:
            wm.kill()
            wm.wait(timeout=6)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--session":
        session(Path(sys.argv[2]))
        return
    for command in ("Xvfb", "xdotool", "xprop", "dbus-run-session"):
        if not shutil.which(command):
            raise RuntimeError(f"Required test command is unavailable: {command}")
    with tempfile.TemporaryDirectory(prefix="midnight-shortcuts-") as temporary:
        root = Path(temporary)
        for name in ("home", "config/midnight-wm", "config-system", "data", "data-system", "runtime"):
            (root / name).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        config = root / "config/midnight-wm/config.ini"
        config.write_text("[Session]\nstart_xscreensaver=false\n[Shortcuts]\n")
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                                 "1280x720x24", "-noreset", "-nolisten", "tcp"], pass_fds=(write_fd,),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                start_new_session=True)
        os.close(write_fd)
        try:
            with os.fdopen(read_fd) as stream:
                display = stream.readline().strip()
            assert display, "Xvfb did not start"
            environment = os.environ.copy()
            for name in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID"):
                environment.pop(name, None)
            environment.update(DISPLAY=":" + display, HOME=str(root / "home"),
                               XDG_CONFIG_HOME=str(root / "config"), XDG_CONFIG_DIRS=str(root / "config-system"),
                               XDG_DATA_HOME=str(root / "data"), XDG_DATA_DIRS=str(root / "data-system"),
                               XDG_RUNTIME_DIR=str(root / "runtime"), LC_ALL="C")
            process = subprocess.Popen(["dbus-run-session", "--", sys.executable, __file__,
                                        "--session", str(config)], env=environment, start_new_session=True)
            try:
                assert process.wait(timeout=50) == 0, "Shortcut smoke failed"
            finally:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=5)
        finally:
            xvfb.terminate()
            xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
