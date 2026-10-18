#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise battery status rendering and polling in isolated Xvfb/D-Bus."""
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
from settings_smoke import run, until, window_for_size, window_position, visible_window


def pixel(image, x, y):
    value = run("convert", str(image), "-crop", f"1x1+{x}+{y}", "-depth", "8",
                "-type", "TrueColor", "txt:-")
    match = re.search(r"#([0-9a-fA-F]{6})", value)
    if match:
        return tuple(int(match.group(1)[offset:offset + 2], 16) for offset in (0, 2, 4))
    raise RuntimeError(f"cannot parse sampled pixel {value!r}")


def set_fixture(path, values):
    temporary = path.with_suffix(".new")
    temporary.write_text(" ".join(str(value) for value in values) + "\n")
    temporary.replace(path)


def session(args):
    set_fixture(args.fixture_data, (1, 75, 90, 1, 0))
    with (args.output / "wm.log").open("w") as log:
        environment = os.environ.copy()
        environment["LD_PRELOAD"] = str(args.fixture)
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT, env=environment)
        try:
            until(lambda: "_NET_SUPPORTING_WM_CHECK(WINDOW)" in
                  run("xprop", "-root", "_NET_SUPPORTING_WM_CHECK"), "window manager did not start")
            other_env = os.environ.copy()
            other = subprocess.run([str(args.binary)], capture_output=True, text=True, timeout=5, env=other_env)
            assert other.returncode != 0, "second WM took over"

            # At 1280x720 with no tray items, the battery fill starts at (1100,12).
            until(lambda: pixel_snapshot(args) == (51, 133, 76), "initial battery did not render")
            run("import", "-window", "root", str(args.output / "battery-75.png"))
            full_fill = pixel(args.output / "battery-75.png", 1109, 12)
            assert full_fill == (51, 133, 76), f"75% battery fill missing at expected panel coordinate: {full_fill}"
            assert pixel(args.output / "battery-75.png", 1111, 12) != (51, 133, 76), \
                "battery fill length did not match 75 percent"

            run("xdotool", "mousemove", "1121", "16", "click", "1")
            notice = until(lambda: window_for_size(420, 100), "battery click did not open its notice")
            run("import", "-window", "root", str(args.output / "battery-notice.png"))
            assert notice
            notice_x, notice_y = window_position(notice)
            run("xdotool", "mousemove", str(notice_x + 210), str(notice_y + 80), "click", "1")

            config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
            settings_env = os.environ.copy()
            # GTK needs the installed loaders, MIME data, and settings schemas.
            # HOME/config remain isolated; the WM retains its empty data dirs.
            settings_env.update(XDG_DATA_DIRS="/usr/local/share:/usr/share", NO_AT_BRIDGE="1")
            settings = subprocess.Popen([str(args.binary.parent / "midnightbsd-settings")], env=settings_env)
            try:
                settings_window = until(lambda: visible_window("Midnight Workspace Settings"),
                                        "settings window did not appear")
                run("xdotool", "windowactivate", "--sync", settings_window)
                run("xdotool", "key", "--clearmodifiers", "ctrl+Page_Down")
                run("xdotool", "key", "--clearmodifiers", "alt+b")
                run("xdotool", "key", "--clearmodifiers", "alt+a")
                until(lambda: "show_battery=false" in config.read_text(), "checkbox did not save disabled state")
                until(lambda: hidden_now(args), "battery did not hide immediately after Apply", timeout=5)
                run("import", "-window", "root", str(args.output / "battery-disabled.png"))
                run("xdotool", "key", "--clearmodifiers", "alt+b")
                run("xdotool", "key", "--clearmodifiers", "alt+a")
                until(lambda: "show_battery=true" in config.read_text(), "checkbox did not save enabled state")
                until(lambda: pixel_snapshot(args) == (51, 133, 76),
                      "battery did not return immediately after Apply", timeout=5)
            finally:
                settings.terminate()
                settings.wait(timeout=5)

            # The panel polls every 30 seconds. Removing the fixture battery must
            # remove its complete group and let the speaker occupy the old gap.
            set_fixture(args.fixture_data, (0, 0, -1, 7, 0))
            until(lambda: hidden_now(args), "battery remained visible after removal", timeout=35)
            run("import", "-window", "root", str(args.output / "battery-absent.png"))
            absent_pixel = pixel(args.output / "battery-absent.png", 1109, 12)
            assert absent_pixel != (51, 133, 76), f"battery fill remained after removal: {absent_pixel}"

            set_fixture(args.fixture_data, (1, 25, 0, 2, 1))
            until(lambda: pixel_snapshot(args) == (51, 133, 76),
                  "charging battery did not return after fixture change", timeout=35)
            run("import", "-window", "root", str(args.output / "battery-charging-25.png"))
            quarter_fill = pixel(args.output / "battery-charging-25.png", 1102, 12)
            assert quarter_fill == (51, 133, 76), f"25% charging fill missing: {quarter_fill}"
            assert pixel(args.output / "battery-charging-25.png", 1105, 12) != (51, 133, 76), \
                "battery fill length did not match 25 percent"

            run("xdotool", "mousemove", "1173", "16", "click", "1")
            popup = until(lambda: window_for_size(260, 230),
                          "speaker popup did not open alongside battery status")
            run("import", "-window", popup, str(args.output / "speaker-popup.png"))
            print("Battery fill, Settings hide/show, notice click, disappearance, charging update and speaker popup passed.")
        finally:
            wm.terminate()
            try:
                wm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                wm.kill()
                wm.wait(timeout=5)


def pixel_snapshot(args):
    screenshot = args.output / "battery-current.png"
    run("import", "-window", "root", str(screenshot))
    return pixel(screenshot, 1100, 12)


def hidden_now(args):
    # An absent battery leaves the panel background at the old fill coordinate.
    screenshot = args.output / "battery-current.png"
    run("import", "-window", "root", str(screenshot))
    return pixel(screenshot, 1109, 12) != (51, 133, 76)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/midnight-wm"))
    parser.add_argument("--fixture", type=Path, default=Path("build/libbattery_sysctl_fixture.so"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--session", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--fixture-data", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.fixture = args.fixture.resolve()
    if args.session:
        session(args)
        return
    for command in ("Xvfb", "dbus-run-session", "xdotool", "xprop", "import", "convert"):
        if not shutil.which(command):
            parser.error(f"required command not found: {command}")
    if not args.binary.is_file():
        parser.error(f"build executable first: {args.binary}")
    if not args.fixture.is_file():
        parser.error(f"build battery_sysctl_fixture first: {args.fixture}")
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-battery-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-battery-") as temporary:
        root = Path(temporary)
        for directory in ("home", "config/midnight-wm", "config-system", "data/applications", "data-system", "runtime"):
            (root / directory).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        (root / "config/midnight-wm/config.ini").write_text("[Workspace]\nui_scale=1.0\n")
        args.fixture_data = root / "battery.fixture"
        set_fixture(args.fixture_data, (1, 75, 90, 1, 0))
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
                for variable in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID", "LD_PRELOAD"):
                    environment.pop(variable, None)
                environment.update(DISPLAY=f":{display}", HOME=str(root / "home"),
                                   XDG_CONFIG_HOME=str(root / "config"), XDG_CONFIG_DIRS=str(root / "config-system"),
                                   XDG_DATA_HOME=str(root / "data"), XDG_DATA_DIRS=str(root / "data-system"),
                                   XDG_RUNTIME_DIR=str(root / "runtime"), MIDNIGHT_TEST_BATTERY_FILE=str(args.fixture_data),
                                   LC_ALL="C")
                process = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                                            "--session", "--binary", str(args.binary), "--fixture", str(args.fixture),
                                            "--fixture-data", str(args.fixture_data), "--output", str(args.output)],
                                           env=environment, start_new_session=True)
                try:
                    if process.wait(timeout=90):
                        raise RuntimeError(f"Battery smoke test failed; see {args.output}")
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
