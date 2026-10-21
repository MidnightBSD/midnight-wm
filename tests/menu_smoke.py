#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check cascading Programs menus in an isolated 720p Xvfb session.

Run after building: python3 tests/menu_smoke.py
Requires Xvfb, D-Bus, xdotool, xwininfo, and ImageMagick import.
"""

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
from settings_smoke import run, until, window_for_size, window_tree, visible_window, atomic_write


def popups():
    # At scale 1 all three launcher levels are 250 pixels wide.
    windows = {}
    for match in re.finditer(r"(0x[0-9a-f]+).*?\b250x(\d+)\+(-?\d+)\+(-?\d+)", window_tree()):
        window, height, x, y = match.groups()
        windows[window] = (int(x), int(y), 250, int(height))
    return windows


def move(x, y):
    run("xdotool", "mousemove", "--sync", str(x), str(y))


def session(args):
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        try:
            until(lambda: window_for_size(1280, 30), "panel did not appear")
            second = subprocess.run([str(args.binary)], capture_output=True, text=True, timeout=10)
            (args.output / "second-wm.log").write_text(second.stdout + second.stderr)
            assert second.returncode != 0, "second WM took over test display"

            def check_launcher_shortcut(target):
                run("xdotool", "windowfocus", "--sync", target)
                run("xdotool", "key", "--clearmodifiers", "ctrl+Escape")
                until(lambda: len(popups()) == 1, "Ctrl+Escape did not open Midnight")
                run("xdotool", "key", "Escape")
                until(lambda: not popups(), "Escape did not dismiss keyboard launcher")
                assert int(run("xdotool", "getwindowfocus").strip()) == int(target, 0), "launcher changed input focus"

            desktop = until(lambda: window_for_size(1280, 720), "desktop missing")
            check_launcher_shortcut(desktop)
            # GTK needs the system MIME/icon data; keep the WM's menu fixtures isolated.
            settings_env = dict(os.environ, XDG_DATA_DIRS="/usr/local/share:/usr/share")
            settings = subprocess.Popen([str(args.binary.parent / "midnightbsd-settings")],
                                        env=settings_env, stdout=log, stderr=subprocess.STDOUT)
            try:
                target = until(lambda: visible_window("Midnight Workspace Settings"), "Settings missing")
                check_launcher_shortcut(target)
            finally:
                settings.terminate()
                settings.wait(timeout=5)
            folder = Path(os.environ["HOME"]) / "shortcut-folder"
            folder.mkdir()
            run("gdbus", "call", "--session", "--dest", "org.freedesktop.FileManager1",
                "--object-path", "/org/freedesktop/FileManager1", "--method",
                "org.freedesktop.FileManager1.ShowFolders", repr([folder.as_uri()]), "")
            target = until(lambda: window_for_size(620, 430), "folder missing")
            check_launcher_shortcut(target)
            # Navigate all three launcher levels without pointer input.
            run("xdotool", "key", "--clearmodifiers", "ctrl+Escape", "Down", "Down", "Down", "Right")
            until(lambda: len(popups()) == 2, "Right did not open Programs")
            run("xdotool", "key", "Down", "Right")  # Development category.
            until(lambda: len(popups()) == 3, "Right did not open the category")
            run("xdotool", "key", "Left")
            until(lambda: len(popups()) == 2, "Left did not return to categories")
            run("xdotool", "key", "Right", "Up", "Down", "Down")  # Wrap in both directions.
            until(lambda: len(popups()) == 3, "Right did not reopen the selected category")
            run("import", "-window", "root", str(args.output / "keyboard-menu.png"))
            run("xdotool", "key", "Return")
            until((Path(os.environ["HOME"]) / "Development-01.launched").exists,
                  "keyboard selection did not launch the second Development application")
            until(lambda: not popups(), "keyboard activation did not dismiss menus")
            # Empty informational submenus must not activate a placeholder.
            run("xdotool", "key", "--clearmodifiers", "ctrl+Escape", "Home", "Down", "Down", "Down", "Return")
            until(lambda: len(popups()) == 2, "Enter did not open Recent Documents")
            run("xdotool", "key", "Down", "Return")
            assert len(popups()) == 2, "empty submenu placeholder was activated"
            run("xdotool", "key", "Left")
            until(lambda: len(popups()) == 1, "Left did not return to root")
            run("xdotool", "key", "Escape")
            until(lambda: not popups(), "Escape did not dismiss menu chain")
            # Mouse opening must support the same keyboard actions; Down wraps to Home.
            run("xdotool", "mousemove", "12", "12", "click", "1")
            until(lambda: bool(popups()), "mouse launcher missing")
            run("xdotool", "key", "End", "Down", "Return")
            until(lambda: visible_window("Folder: " + os.environ["HOME"]), "wrapped selection did not open Home")
            until(lambda: not popups(), "Home activation did not close menu")
            # Rebinding and None must update the global grab after a live reload.
            config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
            run("xdotool", "key", "--clearmodifiers", "ctrl+Escape")
            until(lambda: bool(popups()), "launcher missing before reload")
            atomic_write(config, "[Workspace]\nui_scale=1.0\n[Shortcuts]\nopen_launcher=Super+F6\n")
            until(lambda: not popups(), "reload did not dismiss grabbed menu")
            run("xdotool", "key", "--clearmodifiers", "ctrl+Escape")
            time.sleep(.2)
            assert not popups(), "old launcher binding still active"
            run("xdotool", "key", "--clearmodifiers", "super+F6")
            until(lambda: bool(popups()), "custom launcher binding not active")
            atomic_write(config, "[Workspace]\nui_scale=1.0\n[Shortcuts]\nopen_launcher=None\n")
            until(lambda: not popups(), "disabling shortcut did not reload")
            run("xdotool", "key", "--clearmodifiers", "super+F6", "ctrl+Escape")
            time.sleep(.2)
            assert not popups(), "disabled launcher binding still active"

            def open_categories():
                run("xdotool", "mousemove", "--sync", "12", "12", "click", "1")
                root = until(lambda: next(iter(popups()), None), "launcher did not open")
                run("xdotool", "mousemove", "--sync", "100", "94", "click", "1")  # Programs.
                until(lambda: len(popups()) == 2, "Programs categories did not open")
                until(lambda: all(rect[3] >= 56 for rect in popups().values()),
                      "category reveal did not finish")
                category = next(window for window in popups() if window != root)
                return root, category

            # The direct package-manager entry uses its installed desktop entry's
            # Exec argument vector, including the distribution's privilege wrapper.
            run("xdotool", "mousemove", "12", "12", "click", "1")
            until(lambda: bool(popups()), "launcher did not open for package manager")
            run("xdotool", "mousemove", "100", "286", "click", "1")
            manager_marker = Path(os.environ["HOME"]) / "mport manager.launched"
            until(manager_marker.exists,
                  "Mport Manager desktop entry did not launch")
            assert int(manager_marker.read_text()) == wm.pid, "pkexec lost its window-manager parent"
            until(lambda: not popups(), "package manager launch did not close menu")

            run("xdotool", "mousemove", "12", "12", "click", "1")
            until(lambda: bool(popups()), "launcher did not open for system monitor")
            run("xdotool", "mousemove", "100", "310", "click", "1")
            monitor_marker = Path(os.environ["HOME"]) / "system monitor.launched"
            until(monitor_marker.exists, "System Monitor desktop entry did not launch")
            until(lambda: not popups(), "system monitor launch did not close menu")

            root, category = open_categories()
            cx, cy, _, _ = popups()[category]
            move(cx + 50, cy + 16)
            until(lambda: len(popups()) == 3, "application submenu did not open")
            until(lambda: any(rect[3] == 248 for rect in popups().values()),
                  "application reveal did not reach final height")
            windows = popups()
            app = next(window for window in windows if window not in (root, category))
            assert root in windows and category in windows, "opening submenu lost an earlier level"
            for bounds in windows.values():
                x, y, width, height = bounds
                assert x >= 0 and y >= 30 and x + width <= 1280 and y + height <= 720, bounds
            # Movement inside one category must not recreate/restart its child popup.
            for offset in (60, 90, 120):
                move(cx + offset, cy + 16)
                time.sleep(0.06)
                assert app in popups(), "same-row motion recreated application submenu"
            # Alternate rows rapidly, ending on Development. Only the settled row launches.
            command = ["xdotool"]
            for row in (1, 0, 1, 0, 1):
                command += ["mousemove", str(cx + 80), str(cy + 16 + row * 24), "sleep", "0.02"]
            run(*command)
            time.sleep(0.4)
            assert set(popups()) == {root, category, app}, "category hover replaced popup windows"
            run("import", "-window", "root", str(args.output / "menus.png"))
            ax, ay, _, _ = popups()[app]
            run("xdotool", "mousemove", "--sync", str(ax + 70), str(ay + 16), "click", "1")
            marker = Path(os.environ["HOME"]) / "Development-00.launched"
            until(marker.exists, "settled category did not launch the selected application")
            until(lambda: not popups(), "launch did not close the menu chain")
            assert not list(Path(os.environ["HOME"]).glob("Utility-*.launched")), "stale category launched"

            # Application actions from the desktop entry are available on right-click.
            root, category = open_categories()
            cx, cy, _, _ = popups()[category]
            move(cx + 50, cy + 16)
            until(lambda: len(popups()) == 3, "actions app submenu did not open")
            until(lambda: any(rect[3] == 248 for rect in popups().values()), "actions reveal did not finish")
            app = next(window for window in popups() if window not in (root, category))
            ax, ay, _, _ = popups()[app]
            run("xdotool", "mousemove", str(ax + 70), str(ay + 16), "click", "3")
            until(lambda: len(popups()) == 1, "application context menu did not open")
            x, y, _, _ = next(iter(popups().values()))
            run("xdotool", "mousemove", str(x + 70), str(y + 40), "click", "1")
            until((Path(os.environ["HOME"]) / "Utility-00.action").exists, "named desktop action did not launch")
            until(lambda: not popups(), "action did not close menu")

            # Dismiss immediately after an application popup starts revealing. A pending
            # hover/reveal callback must not resurrect any of the destroyed windows.
            root, category = open_categories()
            cx, cy, _, _ = popups()[category]
            move(cx + 50, cy + 16)
            deadline = time.monotonic() + 5
            observed = None
            while time.monotonic() < deadline:
                windows = popups()
                children = [window for window in windows if window not in (root, category)]
                if children:
                    observed = windows[children[0]][3]
                    break
                time.sleep(0.005)
            assert observed is not None, "second application popup did not appear"
            run("xdotool", "mousemove", "--sync", "1100", "650", "click", "1")
            until(lambda: not popups(), "outside click did not close menus")
            time.sleep(0.5)
            assert not popups(), "pending animation or hover resurrected a closed menu"
            # Reload closes existing menus and cancels any pending reveal/hover.
            root, category = open_categories()
            cx, cy, _, _ = popups()[category]
            move(cx + 50, cy + 16)
            config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
            atomic_write(config, "[Workspace]\nui_scale=1.0\nreduced_motion=true\n")
            until(lambda: not popups(), "live motion reload did not cancel menus")
            time.sleep(.3)
            assert not popups(), "live reload resurrected a menu"
            root, category = open_categories()
            cx, cy, _, _ = popups()[category]
            move(cx + 50, cy + 16)
            deadline = time.monotonic() + 5
            initial = None
            while time.monotonic() < deadline:
                windows = popups()
                children = [w for w in windows if w not in (root, category)]
                if children:
                    initial = windows[children[0]][3]
                    break
            assert initial == 248, f"reduced-motion popup appeared clipped: {initial}"
            atomic_write(config, "[Workspace]\nui_scale=1.0\nreduced_motion=false\n")
            until(lambda: not popups(), "disabling reduced motion did not reload")
            assert wm.poll() is None, "WM exited during menu checks"
            (args.output / "reveal.txt").write_text(f"Observed initial app height: {observed}; final: 248\n")
            print("Mport Manager and System Monitor desktop launch, menu levels, 720p bounds, stable popup IDs, rapid category hover, launch, "
                  "dismissal during reveal, and second-WM rejection passed.")
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
    for command in ("Xvfb", "dbus-run-session", "xdotool", "xwininfo", "import", "touch"):
        if not shutil.which(command):
            parser.error(f"required command not found: {command}")
    if not args.binary.is_file():
        parser.error(f"build executable first: {args.binary}")
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-menu-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-menu-") as temporary:
        root = Path(temporary)
        for directory in ("home", "config/midnight-wm", "config-system", "data/applications", "data-system", "runtime"):
            (root / directory).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        (root / "config/midnight-wm/config.ini").write_text("[Workspace]\nui_scale=1.0\n")
        for category in ("Utility", "Development"):
            for index in range(10):
                name = f"{category}-{index:02}"
                marker = root / "home" / f"{name}.launched"
                (root / "data/applications" / f"{name}.desktop").write_text(
                    f"[Desktop Entry]\nType=Application\nName={name}\n"
                    f'Exec={shutil.which("touch")} "{marker}"\nCategories={category};\n'
                    f'Actions=NewWindow;\n[Desktop Action NewWindow]\nName=New Window\n'
                    f'Exec={shutil.which("touch")} "{root / "home" / (name + ".action")}"\n')
        fake_pkexec = root / "pkexec"
        fake_pkexec.write_text(
            f"#!{sys.executable}\nimport os, pathlib, sys\n"
            "pathlib.Path(sys.argv[1]).write_text(str(os.getppid()))\n")
        fake_pkexec.chmod(0o755)
        (root / "data/applications/mport-manager.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Mport Manager\n"
            f'Exec={fake_pkexec} "{root / "home" / "mport manager.launched"}"\n')
        (root / "data/applications/midnightbsd-system-monitor.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=System Monitor\n"
            f'Exec={shutil.which("touch")} "{root / "home" / "system monitor.launched"}"\n')
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
                        raise RuntimeError(f"Menu smoke test failed; see {args.output}")
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
