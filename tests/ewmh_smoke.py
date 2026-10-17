#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Build and run EWMH protocol checks in isolated Xvfb; never changes the user's session."""
import argparse
import contextlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, window_for_size

def session(args):
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "xcb", "xcb-sync"], text=True).split()
    fixture = args.output / "ewmh-client"
    subprocess.run(["clang++19", "-std=c++23", str(Path(__file__).with_suffix(".cpp")), "-o", str(fixture), *flags], check=True)
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        try:
            time.sleep(1)
            os.kill(wm.pid, signal.SIGPIPE)
            time.sleep(.1)
            assert wm.poll() is None, "SIGPIPE terminated the desktop"
            second = subprocess.run([str(args.binary)], capture_output=True, text=True, timeout=10)
            assert second.returncode != 0, "second WM took over display"
            subprocess.run([str(fixture)], check=True, timeout=35)
            folder = Path(os.environ["HOME"]) / "Workspace folder"
            folder.mkdir()
            def open_folder():
                run("gdbus", "call", "--session", "--timeout", "5", "--dest",
                    "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1",
                    "--method", "org.freedesktop.FileManager1.ShowFolders", repr([folder.as_uri()]), "")
            open_folder()
            folder_window = until(lambda: window_for_size(620, 430), "built-in folder did not open")
            def visible():
                return "Map State: IsViewable" in run("xwininfo", "-id", folder_window)
            until(visible, "folder not visible on its workspace")
            run("xdotool", "set_desktop", "3")
            until(lambda: not visible(), "folder remained visible on another workspace")
            run("xdotool", "set_desktop", "2")
            until(visible, "folder not restored on its workspace")
            run("xdotool", "key", "super+d")
            until(lambda: not visible(), "Show Desktop did not hide built-in folder")
            run("xdotool", "key", "super+d")
            until(visible, "Show Desktop did not restore built-in folder")
            run("xdotool", "key", "super+d")
            until(lambda: not visible(), "second Show Desktop did not hide folder")
            open_folder()
            until(visible, "opening folder did not leave Show Desktop")
            print("Built-in folder workspace visibility and Show Desktop restoration passed.")
            assert wm.poll() is None, "WM exited during protocol tests"
        finally:
            wm.terminate()
            try:
                wm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                wm.kill()
                wm.wait(timeout=5)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/midnight-wm"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--session", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    if args.session:
        session(args)
        return
    for command in ("Xvfb", "dbus-run-session", "xdotool", "clang++19", "pkg-config", "gdbus", "xwininfo", "xmodmap"):
        if not shutil.which(command):
            parser.error(f"required command not found: {command}")
    if not args.binary.is_file():
        parser.error(f"build executable first: {args.binary}")
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-ewmh-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-ewmh-") as temporary:
        root = Path(temporary)
        for directory in ("home", "config/midnight-wm", "config-system", "data/applications", "data-system", "runtime"):
            (root / directory).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        (root / "config/midnight-wm/config.ini").write_text("[Workspace]\nui_scale=1.0\n")
        read_fd, write_fd = os.pipe()
        with (args.output / "xvfb.log").open("w") as log:
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
                                   XDG_CONFIG_HOME=str(root / "config"), XDG_CONFIG_DIRS=str(root / "config-system"),
                                   XDG_DATA_HOME=str(root / "data"), XDG_DATA_DIRS=str(root / "data-system"),
                                   XDG_RUNTIME_DIR=str(root / "runtime"), LC_ALL="C")
                process = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                                            "--session", "--binary", str(args.binary), "--output", str(args.output)],
                                           env=environment, start_new_session=True)
                try:
                    if process.wait(timeout=60):
                        raise RuntimeError(f"EWMH smoke test failed; see {args.output}")
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
