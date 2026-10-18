#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check pager layout properties and navigation in an isolated Xvfb session."""
import os
import contextlib
import signal
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def until(check):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.03)
    raise AssertionError("pager layout state did not update")


def session():
    def layout(values, kind="32c"):
        run("xprop", "-root", "-f", "_NET_DESKTOP_LAYOUT", kind,
            "-set", "_NET_DESKTOP_LAYOUT", values)
        time.sleep(.1)

    def key(name, expected):
        run("xdotool", "key", "ctrl+alt+" + name)
        until(lambda: run("xdotool", "get_desktop") == str(expected))

    # An already running pager has published a vertical layout before WM startup.
    layout("1, 2, 2, 0")
    wm = subprocess.Popen([str(ROOT / "build/midnight-wm")],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        until(lambda: "Midnight Workspace" in run("xprop", "-root", "_NET_DESKTOP_NAMES")
              or "Workspace 1" in run("xprop", "-root", "_NET_DESKTOP_NAMES"))
        assert "1, 2, 2, 0" in run("xprop", "-root", "_NET_DESKTOP_LAYOUT")
        second = subprocess.run([str(ROOT / "build/midnight-wm")], capture_output=True, timeout=10)
        assert second.returncode != 0, "second WM took over display"
        key("Right", 2)
        key("Down", 3)
        layout("0, 2, 2, 2")
        key("Right", 2)
        key("Down", 0)
        layout("0, 2, 0")  # legacy layout, inferred rows and top-left corner
        key("Down", 2)
        for values, kind in (("0, 0, 0, 0", "32c"), ("0, 1, 1, 0", "32c"),
                             ("2, 2, 2, 0", "32c"), ("0, 2, 2, 4", "32c"),
                             ("0, 2", "32c"), ("0, 2, 2, 0, 0", "32c"),
                             ("invalid", "8s")):
            layout(values, kind)
            run("xdotool", "set_desktop", "0")
            until(lambda: run("xdotool", "get_desktop") == "0")
            key("Right", 1)  # malformed property restores horizontal fallback
        layout("1, 2, 2, 0")
        run("xprop", "-root", "-remove", "_NET_DESKTOP_LAYOUT")
        time.sleep(.1)
        key("Right", 2)
        key("Up", 2)
        assert wm.poll() is None
        print("Pager startup, live layouts, corners, legacy data, malformed data, deletion and arrows passed.")
    finally:
        wm.terminate()
        wm.wait(timeout=5)


def main():
    with tempfile.TemporaryDirectory(prefix="midnight-pager-") as temporary:
        root = Path(temporary)
        for name in ("home", "config", "config-system", "data", "data-system", "runtime"):
            (root / name).mkdir()
        (root / "runtime").chmod(0o700)
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                                 "1280x720x24", "-noreset", "-nolisten", "tcp"], pass_fds=(write_fd,),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(write_fd)
        try:
            with os.fdopen(read_fd) as stream:
                display = stream.readline().strip()
            assert display, "Xvfb failed"
            environment = os.environ.copy()
            for name in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID"):
                environment.pop(name, None)
            environment.update(DISPLAY=":" + display, HOME=str(root / "home"),
                               XDG_CONFIG_HOME=str(root / "config"), XDG_CONFIG_DIRS=str(root / "config-system"),
                               XDG_DATA_HOME=str(root / "data"), XDG_DATA_DIRS=str(root / "data-system"),
                               XDG_RUNTIME_DIR=str(root / "runtime"), LC_ALL="C")
            process = subprocess.Popen(["dbus-run-session", "--", "python3", str(Path(__file__).resolve()), "--session"],
                                       env=environment, start_new_session=True)
            try:
                if process.wait(timeout=45):
                    raise RuntimeError("pager smoke test failed")
            finally:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=5)
        finally:
            xvfb.terminate()
            xvfb.wait(timeout=5)


if __name__ == "__main__":
    import sys
    session() if "--session" in sys.argv else main()
