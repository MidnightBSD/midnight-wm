#!/usr/bin/env python3
"""Exercise Midnight Workspace at its supported X11 resolutions."""

import argparse
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile


SIZES = [(1280, 720), (1920, 1080), (2560, 1440), (3440, 1440), (3840, 2160)]


def scale_for(height):
    return 2.0 if height >= 1800 else 1.25 if height >= 1300 else 1.0


def scaled(value, scale):
    return math.floor(value * scale + 0.5)


def run_size(binary, output, width, height, live_resize, config_file=None):
    name = f"{width}x{height}"
    with tempfile.TemporaryDirectory(prefix="midnight-resolution-") as temporary:
        root = Path(temporary)
        for entry in ("home", "config", "config-system", "data", "runtime", "folder"):
            (root / entry).mkdir()
        (root / "runtime").chmod(0o700)
        if config_file:
            (root / "config" / "midnight-wm").mkdir()
            shutil.copyfile(config_file, root / "config" / "midnight-wm" / "config.ini")
        for index in range(70):
            (root / "folder" / f"item-{index:02}.txt").write_text("resolution check\n")

        read_fd, write_fd = os.pipe()
        with (output / f"{name}-xvfb.log").open("w") as log:
            xvfb = subprocess.Popen(
                ["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                 f"{width}x{height}x24", "-nolisten", "tcp"],
                pass_fds=(write_fd,), stdout=log, stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            os.close(write_fd)
            try:
                with os.fdopen(read_fd) as display_pipe:
                    display = display_pipe.readline().strip()
                if not display:
                    raise RuntimeError(f"Xvfb did not start at {name}")
                environment = os.environ.copy()
                environment.pop("DBUS_SESSION_BUS_ADDRESS", None)
                environment.pop("SSH_AUTH_SOCK", None)
                environment.update({
                    "DISPLAY": f":{display}", "HOME": str(root / "home"),
                    "XDG_RUNTIME_DIR": str(root / "runtime"),
                    "XDG_CONFIG_HOME": str(root / "config"),
                    "XDG_CONFIG_DIRS": str(root / "config-system"),
                    "XDG_DATA_HOME": str(root / "data"),
                    "MIDNIGHT_CHECK_BINARY": str(binary),
                    "MIDNIGHT_CHECK_OUTPUT": str(output),
                    "MIDNIGHT_CHECK_NAME": name,
                    "MIDNIGHT_CHECK_FOLDER_URI": (root / "folder").as_uri(),
                    "MIDNIGHT_CHECK_ITEM_URI": (root / "folder" / "item-00.txt").as_uri(),
                    "MIDNIGHT_CHECK_RESIZE": "1" if live_resize else "0",
                    "MIDNIGHT_CHECK_RESIZE_EVENT": str(binary.parent / "screen_resize_event"),
                    # The window manager ignores synthetic root resizes unless
                    # this test-only switch is set.
                    "MIDNIGHT_WM_TEST_SYNTHETIC_RESIZE": "1" if live_resize else "0",
                })
                session_script = r'''
"$MIDNIGHT_CHECK_BINARY" >"$MIDNIGHT_CHECK_OUTPUT/$MIDNIGHT_CHECK_NAME-wm.log" 2>&1 &
wm_pid=$!
trap 'kill "$wm_pid" 2>/dev/null; wait "$wm_pid" 2>/dev/null' EXIT
sleep 2
gdbus call --session --timeout 5 --dest org.freedesktop.FileManager1 \
  --object-path /org/freedesktop/FileManager1 \
  --method org.freedesktop.FileManager1.ShowFolders \
  "['$MIDNIGHT_CHECK_FOLDER_URI']" '' >"$MIDNIGHT_CHECK_OUTPUT/$MIDNIGHT_CHECK_NAME-folder.log" 2>&1 || exit 3
sleep 1
xwininfo -root -tree >"$MIDNIGHT_CHECK_OUTPUT/$MIDNIGHT_CHECK_NAME-windows.txt" || exit 4
import -window root "$MIDNIGHT_CHECK_OUTPUT/$MIDNIGHT_CHECK_NAME.png" || exit 5
xdotool mousemove --sync 12 12 click 1
sleep 1
import -window root "$MIDNIGHT_CHECK_OUTPUT/$MIDNIGHT_CHECK_NAME-menu.png" || exit 10
xdotool mousemove --sync 900 650 click 1
gdbus call --session --timeout 5 --dest org.freedesktop.FileManager1 \
  --object-path /org/freedesktop/FileManager1 \
  --method org.freedesktop.FileManager1.ShowItemProperties \
  "['$MIDNIGHT_CHECK_ITEM_URI']" '' >"$MIDNIGHT_CHECK_OUTPUT/$MIDNIGHT_CHECK_NAME-properties.log" 2>&1 || exit 11
sleep 1
import -window root "$MIDNIGHT_CHECK_OUTPUT/$MIDNIGHT_CHECK_NAME-properties.png" || exit 12
if [ "$MIDNIGHT_CHECK_RESIZE" = 1 ]; then
  if xrandr --fb 1280x720 >"$MIDNIGHT_CHECK_OUTPUT/live-resize.log" 2>&1; then
    printf 'RandR\n' >"$MIDNIGHT_CHECK_OUTPUT/live-resize-type.txt"
    sleep 1
    xwininfo -root -tree >"$MIDNIGHT_CHECK_OUTPUT/live-resize-windows.txt" || exit 6
    import -window root "$MIDNIGHT_CHECK_OUTPUT/live-resize.png" || exit 7
  else
    "$MIDNIGHT_CHECK_RESIZE_EVENT" 1280 720 || exit 8
    printf 'synthetic ConfigureNotify\n' >"$MIDNIGHT_CHECK_OUTPUT/live-resize-type.txt"
    sleep 1
    xwininfo -root -tree >"$MIDNIGHT_CHECK_OUTPUT/live-resize-windows.txt" || exit 9
  fi
fi
'''
                session = subprocess.Popen(
                    ["dbus-run-session", "--", "sh", "-c", session_script],
                    env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    text=True, start_new_session=True,
                )
                try:
                    stdout, stderr = session.communicate(timeout=35)
                except subprocess.TimeoutExpired as exc:
                    os.killpg(session.pid, signal.SIGTERM)
                    session.communicate(timeout=5)
                    raise RuntimeError(f"{name} session timed out") from exc
                (output / f"{name}-session.log").write_text(stdout + stderr)
                if session.returncode:
                    raise RuntimeError(f"{name} session failed ({session.returncode})")
            finally:
                if xvfb.poll() is None:
                    os.killpg(xvfb.pid, signal.SIGTERM)
                xvfb.wait(timeout=5)

    tree = (output / f"{name}-windows.txt").read_text()
    ui_scale = scale_for(height)
    panel_height = scaled(30, ui_scale)
    panel = rf"\b{width}x{panel_height}\+0\+0\b"
    folder = rf"\b{scaled(620, ui_scale)}x{scaled(430, ui_scale)}\+\d+\+\d+\b"
    if not re.search(panel, tree) or not re.search(folder, tree):
        raise RuntimeError(f"{name} panel or folder geometry differs from expected; see window tree")
    if live_resize:
        resized = (output / "live-resize-windows.txt").read_text()
        if not re.search(r"\b1280x30\+0\+0\b", resized):
            raise RuntimeError("live resize did not reflow the panel to 1280x30")
        for window_width, window_height in ((620, 430), (500, 220)):
            geometry = re.search(
                rf"\b{window_width}x{window_height}\+(\d+)\+(\d+)\b", resized
            )
            if not geometry:
                raise RuntimeError(f"live resize lost the {window_width}x{window_height} window")
            x, y = map(int, geometry.groups())
            if x + window_width > 1280 or y + window_height > 720 or y < 30:
                raise RuntimeError(f"live resize left a window outside 1280x720")
        return (output / "live-resize-type.txt").read_text().strip() + " resize passed"
    return "geometry passed"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/midnight-wm"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--config", type=Path, help="Theme configuration to test (keep automatic ui_scale)")
    args = parser.parse_args()
    if args.config and not args.config.is_file():
        parser.error(f"config not found: {args.config}")
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"binary not found: {binary}")
    if not (binary.parent / "screen_resize_event").is_file():
        parser.error("build/screen_resize_event is needed for the Xvfb resize fallback")
    for command in ("Xvfb", "dbus-run-session", "gdbus", "xwininfo", "import", "xdotool"):
        if not shutil.which(command):
            parser.error(f"required program not found: {command}")
    output = args.output or Path(tempfile.mkdtemp(prefix="midnight-resolution-results-"))
    output.mkdir(parents=True, exist_ok=True)
    output = output.resolve()
    print(f"Results: {output}", flush=True)
    for width, height in SIZES:
        result = run_size(binary, output, width, height, width == 3840, args.config)
        print(f"{width}x{height}: {result}", flush=True)
    if (output / "live-resize-type.txt").read_text().startswith("synthetic"):
        print("Xvfb has no smaller RandR mode; internal reflow was checked with ConfigureNotify.")


if __name__ == "__main__":
    main()
