#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check real Picom shadows and live Settings on an isolated X11 display."""
import argparse
import contextlib
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, visible_window, window_for_size
from battery_smoke import pixel


def session(args):
    config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
    def configure(enabled):
        temporary = config.with_suffix(".new")
        temporary.write_text("[Workspace]\nui_scale=1.0\ndesktop_image=\n"
                             "desktop_color=#7866a8\ncompositing=" + str(enabled).lower() + "\n")
        temporary.replace(config)
    def owner():
        return int(run(str(args.binary.parent / "compositor_test"), "--owner").strip())
    def capture(name):
        path = args.output / (name + ".png")
        run("import", "-window", "root", str(path))
        return path
    def below(window):
        info = run("xwininfo", "-id", window)
        x = int(re.search(r"Absolute upper-left X:\s+(-?\d+)", info)[1])
        y = int(re.search(r"Absolute upper-left Y:\s+(-?\d+)", info)[1])
        w = int(re.search(r"Width:\s+(\d+)", info)[1])
        h = int(re.search(r"Height:\s+(\d+)", info)[1])
        return x + w // 2, y + h + 3

    configure(False)
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        terminal = settings = None
        try:
            until(lambda: window_for_size(1280, 30), "panel missing")
            assert owner() == 0, "disabled compositing started Picom"
            second = subprocess.run([str(args.binary)], capture_output=True, timeout=5)
            assert second.returncode != 0, "second WM took over"
            terminal = subprocess.Popen(["xterm", "-title", "Compositor example", "-geometry", "50x12+160+160"])
            client = until(lambda: visible_window("Compositor example"), "terminal missing")
            tree = run("xwininfo", "-id", client, "-tree")
            frame = re.search(r"Parent window id:\s+(0x[0-9a-f]+)", tree)[1]
            sample = below(frame)
            baseline = capture("uncomposited")
            configure(True)
            until(lambda: owner() != 0, "Picom did not claim compositor selection")
            time.sleep(.5)
            shadowed = capture("window-shadow")
            before, after = pixel(baseline, *sample), pixel(shadowed, *sample)
            assert sum(after) < sum(before) - 15, f"window shadow missing: {before} -> {after}"
            assert pixel(shadowed, 1000, 33) == pixel(baseline, 1000, 33), "panel has an unwanted shadow"

            folder = Path(os.environ["HOME"]) / "Folder"
            folder.mkdir()
            run("gdbus", "call", "--session", "--dest", "org.freedesktop.FileManager1",
                "--object-path", "/org/freedesktop/FileManager1", "--method",
                "org.freedesktop.FileManager1.ShowFolders", repr([folder.as_uri()]), "")
            folder_window = until(lambda: window_for_size(620, 430), "built-in folder missing")
            folder_sample = below(folder_window)
            time.sleep(.5)
            folders = capture("folder-shadow")
            assert sum(pixel(folders, *folder_sample)) < sum((120, 102, 168)) - 15, "folder shadow missing"

            settings = subprocess.Popen([str(args.binary.parent / "midnightbsd-settings")])
            dialog = until(lambda: visible_window("Midnight Workspace Settings"), "Settings missing")
            run("xdotool", "windowactivate", "--sync", dialog)
            run("xdotool", "key", "--clearmodifiers", "alt+s")
            run("xdotool", "key", "--clearmodifiers", "alt+a")
            until(lambda: "compositing=false" in config.read_text(), "Settings did not save compositor toggle")
            until(lambda: owner() == 0, "Settings did not stop owned compositor")
            run("xdotool", "key", "--clearmodifiers", "alt+s")
            run("xdotool", "key", "--clearmodifiers", "alt+a")
            until(lambda: owner() != 0, "Settings did not restart compositor")
            settings.terminate()
            settings.wait(timeout=5)
            settings = None
            assert wm.poll() is None, "WM exited while toggling compositing"
            wm.terminate()
            wm.wait(timeout=5)
            until(lambda: owner() == 0, "Picom survived WM shutdown")
            print("Managed-window/folder shadows, panel exclusion, live Settings, second-WM rejection and shutdown passed.")
        finally:
            for process in (settings, terminal, wm):
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/midnight-wm"))
    parser.add_argument("--picom", type=Path, default=shutil.which("picom"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--session", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    if args.session:
        session(args)
        return
    if not args.picom or not args.picom.is_file():
        parser.error("Picom is required; install it or pass --picom /path/to/picom")
    args.picom = args.picom.resolve()
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-compositor-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-compositor-") as tmp:
        root = Path(tmp)
        for path in ("home", "config/midnight-wm", "config-system", "data", "runtime", "bin"):
            (root / path).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        (root / "bin/picom").symlink_to(args.picom)
        r, w = os.pipe()
        with (args.output / "xvfb.log").open("w") as log:
            xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", "1280x720x24",
                                     "-nolisten", "tcp"], pass_fds=(w,), stdout=log, stderr=subprocess.STDOUT)
            os.close(w)
            try:
                with os.fdopen(r) as stream:
                    display = stream.readline().strip()
                if not display:
                    raise RuntimeError("Xvfb did not start")
                env = os.environ.copy()
                for key in ("DBUS_SESSION_BUS_ADDRESS", "SSH_AUTH_SOCK", "SSH_AGENT_PID"):
                    env.pop(key, None)
                env.update(DISPLAY=":" + display, HOME=str(root / "home"),
                           XDG_CONFIG_HOME=str(root / "config"), XDG_CONFIG_DIRS=str(root / "config-system"),
                           XDG_DATA_HOME=str(root / "data"), XDG_RUNTIME_DIR=str(root / "runtime"),
                           PATH=str(root / "bin") + os.pathsep + env.get("PATH", ""), NO_AT_BRIDGE="1")
                process = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                                            "--session", "--binary", str(args.binary), "--output", str(args.output)],
                                           env=env, start_new_session=True)
                try:
                    if process.wait(timeout=60):
                        raise RuntimeError("Compositor smoke failed; see " + str(args.output))
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
