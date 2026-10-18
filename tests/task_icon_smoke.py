#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check running application taskbar icons in an isolated X11 session."""
import argparse
import contextlib
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, visible_window, window_for_size
from battery_smoke import pixel


def session(args):
    flags = run("pkg-config", "--cflags", "--libs", "xcb").split()
    fixture = args.output / "task-icon-client"
    subprocess.run(["clang++19", "-std=c++23", str(Path(__file__).with_name("task_icon_client.cpp")),
                    "-o", str(fixture), *flags], check=True)
    def capture(name):
        path = args.output / (name + ".png")
        run("import", "-window", "root", str(path))
        return path
    def task_color():
        return pixel(capture("current"), 225, 17)
    def send(command):
        client.stdin.write(command + "\n")
        client.stdin.flush()
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        client = None
        try:
            until(lambda: window_for_size(1280, 30), "panel missing")
            second = subprocess.run([str(args.binary)], capture_output=True, timeout=5)
            assert second.returncode != 0, "second WM took over"
            client = subprocess.Popen([str(fixture)], stdin=subprocess.PIPE, text=True)
            first = until(lambda: visible_window("Task icon 1"), "task fixture missing")
            until(lambda: task_color() == (255, 32, 32), "initial application icon missing")
            capture("task-icons")
            send("green")
            until(lambda: task_color() == (32, 255, 32), "live icon update missing")
            send("delete")
            until(lambda: task_color() == (32, 64, 255), "desktop entry class fallback missing")
            capture("class-fallback")
            send("malformed")
            until(lambda: task_color() == (32, 64, 255), "malformed image did not retain fallback")
            send("class")
            until(lambda: task_color() != (32, 64, 255), "WM_CLASS change did not refresh fallback")
            capture("generic-fallback")
            run("xdotool", "mousemove", "240", "16", "click", "1")
            until(lambda: run("xdotool", "getactivewindow").strip() == first, "task activation failed")
            # With the battery hidden there are seven visible 110px tasks at 720p.
            run("xdotool", "mousemove", "1022", "16", "click", "1")
            time.sleep(.3)
            overflow = capture("overflow-icons")
            histogram = run("convert", str(overflow), "-crop", "1280x500+0+30", "-format", "%c", "histogram:info:-")
            assert "#FF2020" in histogram.upper(), "overflow menu did not show app icons"
            run("xdotool", "key", "Escape")
            send("quit")
            client.wait(timeout=5)
            until(lambda: visible_window("Task icon 1") is None, "destroyed task was retained")
            assert wm.poll() is None, "WM exited during icon updates"
            print("Task icons, live changes, class fallback, malformed input, activation, overflow and cleanup passed.")
        finally:
            for process in (client, wm):
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)


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
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-task-icons-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-task-icons-") as tmp:
        root = Path(tmp)
        for path in ("home", "config/midnight-wm", "config-system", "data", "runtime", "bin"):
            (root / path).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        (root / "config/midnight-wm/config.ini").write_text("[Workspace]\nui_scale=1.0\nshow_battery=false\ncompositing=false\n")
        apps = root / "data/applications"
        apps.mkdir()
        (apps / "task-icon-fixture.desktop").write_text("[Desktop Entry]\nType=Application\nName=Task icon fixture\nExec=true\nStartupWMClass=TaskIconFixture\nIcon=task-icon-fixture\n")
        icons = root / "data/icons/hicolor"
        (icons / "48x48/apps").mkdir(parents=True)
        (icons / "index.theme").write_text("[Icon Theme]\nName=Hicolor\nDirectories=48x48/apps\n[48x48/apps]\nSize=48\nType=Fixed\nContext=Applications\n")
        (icons / "48x48/apps/task-icon-fixture.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48"><path fill="#2040ff" d="M0 0h48v48H0z"/></svg>')
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
                        raise RuntimeError("Task icon smoke failed; see " + str(args.output))
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
