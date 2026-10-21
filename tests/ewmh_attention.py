#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Build and run EWMH attention and stacking checks in isolated Xvfb; never changes the user's session."""
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
from settings_smoke import run, until, window_for_size, visible_window, geometry, window_position

def session(args):
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "xcb", "xcb-xinerama"], text=True).split()
    fixture = args.output / "ewmh-attention-client"
    subprocess.run(["clang++19", "-std=c++23", str(Path(__file__).with_suffix(".cpp")), "-o", str(fixture), *flags], check=True)
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        try:
            time.sleep(1)
            second = subprocess.run([str(args.binary)], capture_output=True, text=True, timeout=10)
            assert second.returncode != 0, "second WM took over display"
            subprocess.run([str(fixture), *(["--monitors-only"] if args.multi_monitor else [])], check=True, timeout=45)
            if args.multi_monitor:
                assert wm.poll() is None, "WM exited during monitor protocol tests"
                return
            # Check the visible taskbar marker without driving another desktop.
            apps = []
            try:
                app = subprocess.Popen([str(fixture), "--visual"])
                apps.append(app)
                first_window = until(lambda: visible_window("Attention example"), "attention example missing")
                second_window = until(lambda: visible_window("Foreground example"), "foreground example missing")
                until(lambda: "_NET_WM_STATE_SHADED" in run("xprop", "-id", first_window, "_NET_WM_STATE"),
                      "visual example not initially shaded")
                run("xdotool", "windowactivate", "--sync", second_window)
                run("xdotool", "set_window", "--urgency", "1", first_window)
                until(lambda: "_NET_WM_STATE_DEMANDS_ATTENTION" in run("xprop", "-id", first_window, "_NET_WM_STATE"),
                      "urgency not exported")
                time.sleep(.2)
                run("import", "-window", "root", str(args.output / "attention-taskbar.png"))
                histogram = run("convert", str(args.output / "attention-taskbar.png"), "-crop", "1280x30+0+0", "-format", "%c", "histogram:info:-")
                assert "#854005" in histogram.upper(), "amber task attention marker missing"
                assert run("xdotool", "getactivewindow").strip() == second_window, "attention changed active window"
                # The first application's task button is selected explicitly.
                run("xdotool", "mousemove", "240", "16", "click", "1")
                until(lambda: "_NET_WM_STATE_DEMANDS_ATTENTION" not in run("xprop", "-id", first_window, "_NET_WM_STATE"),
                      "task click did not clear attention")
                until(lambda: "_NET_WM_STATE_SHADED" not in run("xprop", "-id", first_window, "_NET_WM_STATE"),
                      "task click did not unshade window")
                run("import", "-window", "root", str(args.output / "attention-cleared.png"))
                third_window = until(lambda: visible_window("Third example"), "third example missing")
                # Exercise passive shortcut grabs using real keyboard events.
                def key(*keys):
                    run("xdotool", "key", "--clearmodifiers", *keys)
                def active(window):
                    return run("xdotool", "getactivewindow").strip() == window
                key("alt+shift+Tab")
                until(lambda: active(third_window), "reverse shortcut did not select previous window")
                key("alt+Tab")
                until(lambda: active(first_window), "forward shortcut did not reverse the cycle")
                original = window_position(first_window)
                size = geometry(first_window)
                key("alt+F7", "Right", "ctrl+Down", "Return")
                until(lambda: window_position(first_window) == (original[0] + 10, original[1] + 1),
                      "keyboard move did not apply coarse/fine steps")
                moved = window_position(first_window)
                key("alt+F7", "Left", "Escape")
                until(lambda: window_position(first_window) == moved, "move cancellation lost geometry")
                key("alt+F8", "Right", "ctrl+Down", "Return")
                until(lambda: geometry(first_window) == (size[0] + 10, size[1] + 1),
                      "keyboard resize did not apply coarse/fine steps")
                resized = geometry(first_window)
                key("alt+F8", "Down", "Escape")
                until(lambda: geometry(first_window) == resized, "resize cancellation lost geometry")
                key("alt+F10")
                until(lambda: "MAXIMIZED_VERT" in run("xprop", "-id", first_window, "_NET_WM_STATE"),
                      "maximize shortcut failed")
                maximized = (window_position(first_window), geometry(first_window))
                key("alt+F7", "Right", "Return", "alt+F8", "Down", "Return")
                assert (window_position(first_window), geometry(first_window)) == maximized, "maximized window moved/resized"
                key("alt+F10")
                until(lambda: (window_position(first_window), geometry(first_window)) == (moved, resized),
                      "maximize toggle did not restore geometry")
                key("alt+F9")
                until(lambda: "HIDDEN" in run("xprop", "-id", first_window, "_NET_WM_STATE"),
                      "minimize shortcut failed")
                key("alt+shift+Tab")
                until(lambda: active(third_window), "switch did not skip minimized window")
                # A config reload changes passive grabs, including None disabling.
                from settings_smoke import atomic_write
                config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
                atomic_write(config, "[Workspace]\nui_scale=1.25\n[Shortcuts]\nmove_window=Alt+F6\nresize_window=None\n")
                until(lambda: window_for_size(1280, 38), "shortcut config did not reload")
                before = window_position(third_window)
                key("alt+F7", "Right", "Return")
                assert window_position(third_window) == before, "old move binding remained grabbed"
                key("alt+F6", "Right", "Return")
                until(lambda: window_position(third_window) == (before[0] + 10, before[1]),
                      "new move binding did not apply live")
                before_size = geometry(third_window)
                key("alt+F8", "Down", "Return")
                assert geometry(third_window) == before_size, "disabled resize still operated"
                # Destroy a client during a keyboard operation; grabs must be released.
                key("alt+F6")
                app.terminate()
                app.wait(timeout=5)
                key("ctrl+alt+Right")
                until(lambda: "= 1" in run("xprop", "-root", "_NET_CURRENT_DESKTOP"),
                      "destroyed client left keyboard grabbed")
                print("Window operation shortcuts passed.")
                print("Task attention marker and explicit task activation passed.")
            finally:
                for app in apps:
                    app.terminate()
                    app.wait(timeout=5)
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
    parser.add_argument("--multi-monitor", action="store_true", help="check Xinerama fullscreen edges on two overlapping Xvfb screens")
    parser.add_argument("--session", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    if args.session:
        session(args)
        return
    for command in ("Xvfb", "dbus-run-session", "xdotool", "clang++19", "pkg-config", "gdbus", "xwininfo", "xmodmap", "xprop", "import", "convert"):
        if not shutil.which(command):
            parser.error(f"required command not found: {command}")
    if not args.binary.is_file():
        parser.error(f"build executable first: {args.binary}")
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-attention-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-attention-") as temporary:
        root = Path(temporary)
        for directory in ("home", "config/midnight-wm", "config-system", "data/applications", "data-system", "runtime"):
            (root / directory).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        (root / "config/midnight-wm/config.ini").write_text("[Workspace]\nui_scale=1.0\n")
        read_fd, write_fd = os.pipe()
        with (args.output / "xvfb.log").open("w") as log:
            xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                                     "1280x720x24", *(["-screen", "1", "800x600x24", "+xinerama"] if args.multi_monitor else []),
                                     "-nolisten", "tcp"], pass_fds=(write_fd,),
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
                                            "--session", "--binary", str(args.binary), "--output", str(args.output),
                                            *(["--multi-monitor"] if args.multi_monitor else [])],
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
