#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check server-side desktop restoration with the WM deliberately suspended.

Run after building: python3 tests/desktop_repaint_smoke.py
Uses an isolated Xvfb/D-Bus session; artifacts remain in --output.
"""

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

from settings_smoke import atomic_write, run, until, window_for_size


def pixel(output, name):
    snapshot = output / f"{name}.png"
    run("import", "-window", "root", str(snapshot))
    return run("convert", str(snapshot), "-crop", "1x1+500+300", "+repage",
               "-depth", "8", "txt:-").upper()


def session(args):
    config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
    folder = Path(os.environ["HOME"]) / "Occluding folder"
    folder.mkdir()
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        try:
            until(lambda: window_for_size(1280, 30), "panel did not appear")
            until(lambda: "#00FF00" in pixel(args.output, "initial"), "wallpaper did not appear")
            second = subprocess.run([str(args.binary)], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=10)
            (args.output / "second-wm.log").write_text(second.stdout)
            assert second.returncode != 0, "second WM unexpectedly took over the display"
            run("gdbus", "call", "--session", "--timeout", "5", "--dest",
                "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1",
                "--method", "org.freedesktop.FileManager1.ShowFolders", repr([folder.as_uri()]), "")
            window = until(lambda: window_for_size(620, 430), "folder did not appear")
            until(lambda: "#00FF00" not in pixel(args.output, "occluded"), "folder did not cover test pixel")

            def expose(name, expected):
                # With the event loop stopped, only the X server can restore pixels.
                wm.send_signal(signal.SIGSTOP)
                try:
                    time.sleep(0.15)
                    run("xdotool", "windowunmap", window)
                    actual = pixel(args.output, name)
                    assert expected in actual, f"{name}: exposed desktop pixel is wrong: {actual}"
                finally:
                    wm.send_signal(signal.SIGCONT)

            expose("wallpaper-exposed", "#00FF00")
            # A live reload must replace the server background as well as the visible image.
            atomic_write(config, "[Workspace]\nui_scale=1.0\ndesktop_color=#123456\ndesktop_image=\n")
            until(lambda: "#123456" in pixel(args.output, "color-reloaded"), "solid color reload failed")
            run("xdotool", "windowmap", window)
            until(lambda: "#123456" not in pixel(args.output, "color-occluded"), "folder did not remap")
            expose("color-exposed", "#123456")
            assert wm.poll() is None, "WM exited during desktop redraw checks"
            print("Wallpaper and live solid-color restoration passed with WM suspended; second WM rejected.")
        finally:
            with contextlib.suppress(ProcessLookupError):
                wm.send_signal(signal.SIGCONT)
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
    for command in ("Xvfb", "dbus-run-session", "gdbus", "xwininfo", "xdotool", "import", "convert"):
        if not shutil.which(command):
            parser.error(f"required command not found: {command}")
    if not args.binary.is_file():
        parser.error(f"build executable first: {args.binary}")
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-repaint-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-repaint-") as temporary:
        root = Path(temporary)
        for directory in ("home", "config/midnight-wm", "config-system", "data", "runtime"):
            (root / directory).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        image = root / "wallpaper.svg"
        image.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="720">'
                         '<rect width="1280" height="720" fill="#00ff00"/></svg>')
        (root / "config/midnight-wm/config.ini").write_text(
            f"[Workspace]\nui_scale=1.0\ndesktop_color=#ff00ff\ndesktop_image={image}\n")
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
                                   XDG_DATA_HOME=str(root / "data"), XDG_RUNTIME_DIR=str(root / "runtime"))
                process = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                                            "--session", "--binary", str(args.binary), "--output", str(args.output)],
                                           env=environment, start_new_session=True)
                try:
                    if process.wait(timeout=60):
                        raise RuntimeError(f"Desktop repaint check failed; see {args.output}")
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGCONT)
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
