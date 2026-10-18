#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check Settings and live configuration in a disposable Xvfb/D-Bus session.

Run manually after building: python3 tests/settings_smoke.py
Artifacts are kept in --output (or a printed temporary results directory).
This intentionally is not a CTest: it requires Xvfb, xdotool, and ImageMagick.
"""

import argparse
import configparser
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


def run(*arguments, **kwargs):
    return subprocess.check_output(arguments, text=True, timeout=10, **kwargs)


def until(predicate, message, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.15)
    raise RuntimeError(message)


def window_tree():
    return run("xwininfo", "-root", "-tree")


def window_for_size(width, height):
    match = re.search(rf"(0x[0-9a-f]+).*?\b{width}x{height}\+", window_tree())
    return match.group(1) if match else None


def geometry(window):
    info = run("xwininfo", "-id", window)
    return tuple(int(re.search(rf"{key}:\s+(\d+)", info).group(1))
                 for key in ("Width", "Height"))


def frame_extents(window):
    value = run("xprop", "-id", window, "_NET_FRAME_EXTENTS")
    match = re.search(r"=\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)", value)
    return tuple(map(int, match.groups())) if match else None


def window_position(window):
    info = run("xwininfo", "-id", window)
    return tuple(int(re.search(rf"Absolute upper-left {axis}:\s+(-?\d+)", info).group(1))
                 for axis in ("X", "Y"))


def visible_window(title):
    result = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", f"^{title}$"],
                            capture_output=True, text=True, timeout=5)
    return result.stdout.splitlines()[0] if result.returncode == 0 and result.stdout.strip() else None


def check_wallpaper_chooser(settings, output, name):
    run("xdotool", "windowactivate", "--sync", settings)
    run("xdotool", "key", "--clearmodifiers", "alt+i")
    chooser = until(lambda: visible_window("Choose desktop background"), "wallpaper chooser did not open")
    extents = until(lambda: frame_extents(chooser), "wallpaper chooser was not decorated")
    (output / f"{name}-hints.txt").write_text(run("xprop", "-id", chooser, "WM_NORMAL_HINTS"))
    # Allow GTK to complete its initial folder load and resulting size requests.
    time.sleep(0.5)
    x, y = window_position(chooser)
    width, height = geometry(chooser)
    left, right, top, bottom = extents
    bounds = (x - left, y - top, x + width + right, y + height + bottom)
    tree = run("xwininfo", "-id", chooser, "-tree")
    frame = re.search(r"Parent window id:\s+(0x[0-9a-f]+)", tree).group(1)
    frame_x, frame_y = window_position(frame)
    frame_w, frame_h = geometry(frame)
    assert (frame_w, frame_h) == (width + left + right, height + top + bottom), \
        "chooser frame did not follow the client resize"
    assert (frame_x, frame_y) == (bounds[0], bounds[1]), "chooser frame and client positions differ"
    run("import", "-window", "root", str(output / f"{name}.png"))
    assert bounds[0] >= 0 and bounds[1] >= 30 and bounds[2] <= 1280 and bounds[3] <= 720, \
        f"wallpaper chooser extends outside usable screen: {bounds}"
    run("xdotool", "windowactivate", "--sync", chooser)
    run("xdotool", "key", "--clearmodifiers", "Escape")
    until(lambda: visible_window("Choose desktop background") is None, "wallpaper chooser did not close")


def atomic_write(path, text):
    temporary = path.with_suffix(".new")
    temporary.write_text(text)
    temporary.replace(path)


def write_icon_theme(data_home, name, color):
    icon_dir = data_home / "icons" / name / "18x18" / "places"
    icon_dir.mkdir(parents=True, exist_ok=True)
    (icon_dir.parent.parent / "index.theme").write_text(
        f"[Icon Theme]\nName={name}\nDirectories=18x18/places\n"
        "\n[18x18/places]\nSize=18\nType=Fixed\n")
    run("convert", "-size", "18x18", f"xc:{color}", str(icon_dir / "user-trash.png"))


def check_trash_icon_theme(output, color, label):
    run("xdotool", "mousemove", "--sync", "12", "12", "click", "1")
    time.sleep(0.3)
    image_path = output / f"icon-theme-{label}.png"
    run("import", "-window", "root", str(image_path))
    histogram = run("convert", str(image_path), "-format", "%c", "histogram:info:-")
    match = re.search(r"(?m)^\s*([0-9]+):[^\n]*" + re.escape(color.upper()) + r"(?:[0-9A-F]{2})?\b",
                      histogram.upper())
    assert match and int(match.group(1)) >= 250, \
        f"launcher trash icon did not use {label} theme color {color}"
    run("xdotool", "key", "Escape")


def session(args):
    output = args.output
    config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
    gtk_settings = Path(os.environ["XDG_CONFIG_HOME"]) / "gtk-3.0/settings.ini"
    folder = Path(os.environ["HOME"]) / "Test folder"
    folder.mkdir()
    (folder / "kept.txt").write_text("The folder must stay open during reload.\n")
    with (output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        try:
            panel = until(lambda: window_for_size(1280, 30), "initial panel did not appear")
            # Panel mapping precedes completion of session service startup.
            # Wait for an actual WM event round trip before sending menu input.
            for desktop in (1, 0):
                run("xdotool", "set_desktop", str(desktop))
                until(lambda: f"= {desktop}" in run("xprop", "-root", "_NET_CURRENT_DESKTOP"),
                      "WM did not finish startup event dispatch")
            check_trash_icon_theme(output, "#e02020", "red")
            run("gdbus", "call", "--session", "--timeout", "5", "--dest",
                "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1",
                "--method", "org.freedesktop.FileManager1.ShowFolders", repr([folder.as_uri()]), "")
            folder_window = until(lambda: window_for_size(620, 430), "folder did not open")
            folder_x, folder_y = window_position(folder_window)
            close_x = folder_x + 600
            close_y = folder_y + 15
            for button in ("3", "2"):
                run("xdotool", "mousemove", str(close_x), str(close_y), "click", button)
                time.sleep(0.2)
                assert window_for_size(620, 430) == folder_window, \
                    f"folder close control responded to mouse button {button}"
            run("xdotool", "mousemove", str(close_x), str(close_y), "click", "1")
            until(lambda: window_for_size(620, 430) is None, "left click did not close folder")
            run("gdbus", "call", "--session", "--timeout", "5", "--dest",
                "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1",
                "--method", "org.freedesktop.FileManager1.ShowFolders", repr([folder.as_uri()]), "")
            folder_window = until(lambda: window_for_size(620, 430), "folder did not reopen")
            # The seventh Midnight menu row is Settings. PATH points to the build directory.
            run("xdotool", "mousemove", "--sync", "12", "12", "click", "1")
            time.sleep(0.2)
            run("xdotool", "mousemove", "--sync", "100", "190", "click", "1")
            settings = until(lambda: settings_window(), "Settings did not launch from Midnight menu")
            until(lambda: frame_extents(settings) == (5, 5, 28, 5),
                  "Settings did not receive initial frame extents")
            run("import", "-window", "root", str(output / "settings.png"))
            check_wallpaper_chooser(settings, output, "wallpaper-chooser")
            original_x, original_y = window_position(settings)
            run("xdotool", "windowmove", "--sync", settings, "800", "480")
            check_wallpaper_chooser(settings, output, "wallpaper-chooser-edge")
            run("xdotool", "windowmove", "--sync", settings, str(original_x), str(original_y))
            # Exercise a visible editable field and the Apply button through mnemonics.
            apply_ui(settings)
            until(lambda: "Smoke test" in config.read_text(), "Settings Apply did not save panel label")
            parsed = configparser.ConfigParser(interpolation=None)
            parsed.read(config)
            assert parsed["Unrelated"]["retained"] == "keep this value", "Apply removed unknown settings"
            assert "preserve this comment" in config.read_text(), "Apply removed existing comments"
            assert dict(parsed["Workspace"]) == {
                "ui_scale": "1.0", "panel_label": "Smoke test",
                "ui_font": "Sans Italic 12", "title_font": "Sans Bold 13",
            }, "Label-only Apply changed fonts or introduced default/empty overrides"
            until(lambda: geometry(panel) == (1280, 30), "unexpected UI apply panel geometry")
            assert wm.poll() is None, "WM exited during Settings Apply"
            assert geometry(folder_window) == (620, 430), "Settings Apply lost the existing folder"

            # External atomic saves must follow the same reload path as the Settings app.
            atomic_write(gtk_settings, "[Settings]\ngtk-icon-theme-name=SmokeBlue\n")
            changed = ("[Workspace]\npanel_label=Smoke test\nui_scale=1.25\n"
                       "panel_color=#123456\naccent_color=#654321\ndesktop_color=#345678\n"
                       "[Unrelated]\nretained=keep this value\n")
            atomic_write(config, changed)
            until(lambda: geometry(panel) == (1280, 38), "live scale did not resize the existing panel")
            until(lambda: geometry(folder_window) == (775, 538), "live scale did not resize the existing folder")
            until(lambda: frame_extents(settings) == (6, 6, 35, 6),
                  "live scale did not update the existing Settings client's frame extents")
            check_trash_icon_theme(output, "#2040e0", "blue")
            assert settings_window() == settings, "reload replaced the Settings client"
            run("import", "-window", "root", str(output / "settings-reloaded.png"))
            run("xdotool", "windowactivate", "--sync", settings)
            run("xdotool", "key", "--clearmodifiers", "alt+F4")
            until(lambda: settings_window() is None, "Settings did not close")
            run("import", "-window", panel, str(output / "panel-reloaded.png"))
            histogram = run("convert", str(output / "panel-reloaded.png"), "-format", "%c", "histogram:info:-")
            assert "#123456" in histogram.upper(), "panel_color was not applied to the live panel"
            assert "#654321" in histogram.upper(), "accent_color was not applied to the live panel"
            run("import", "-window", "root", str(output / "reloaded.png"))
            atomic_write(config, "[Workspace\nui_scale=3\n")
            time.sleep(3.5)
            assert geometry(panel) == (1280, 38), "malformed config replaced the valid panel configuration"
            assert geometry(folder_window) == (775, 538), "malformed config changed the folder"
            atomic_write(config, changed.replace("ui_scale=1.25", "ui_scale=1.0"))
            until(lambda: geometry(panel) == (1280, 30), "reload did not recover after malformed config")
            assert wm.poll() is None, "reload restarted or exited the original WM process"
            (output / "windows.txt").write_text(window_tree())
            print("Settings menu launch, wallpaper chooser bounds, Apply, preservation, live color/scale, "
                  "and malformed-config recovery passed.")
        finally:
            wm.terminate()
            try:
                wm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                wm.kill()
                wm.wait(timeout=5)


def settings_window():
    return visible_window("Midnight Workspace Settings")


def apply_ui(window):
    run("xdotool", "windowactivate", "--sync", window)
    run("xdotool", "key", "--clearmodifiers", "ctrl+Page_Down")
    run("xdotool", "key", "--clearmodifiers", "alt+m")
    run("xdotool", "key", "--clearmodifiers", "ctrl+a")
    run("xdotool", "type", "--clearmodifiers", "Smoke test")
    run("xdotool", "key", "--clearmodifiers", "alt+a")


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
    for command in ("Xvfb", "dbus-run-session", "gdbus", "xwininfo", "xprop", "xdotool", "import", "convert"):
        if not shutil.which(command):
            parser.error(f"required command not found: {command}")
    for binary in (args.binary, args.binary.parent / "midnightbsd-settings"):
        if not binary.is_file():
            parser.error(f"build executable first: {binary}")
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-settings-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-settings-") as temporary:
        root = Path(temporary)
        for directory in ("home", "config/midnight-wm", "config-system", "data", "runtime"):
            (root / directory).mkdir(parents=True)
        write_icon_theme(root / "data", "SmokeRed", "#e02020")
        write_icon_theme(root / "data", "SmokeBlue", "#2040e0")
        gtk_config = root / "config/gtk-3.0"
        gtk_config.mkdir(parents=True)
        (gtk_config / "settings.ini").write_text(
            "[Settings]\ngtk-icon-theme-name=SmokeRed\n")
        (root / "runtime").chmod(0o700)
        (root / "config/midnight-wm/config.ini").write_text(
            "# preserve this comment\n[Workspace]\nui_scale=1.0\npanel_label=Midnight\n"
            "ui_font=Sans Italic 12\ntitle_font=Sans Bold 13\n"
            "[Unrelated]\nretained=keep this value\n")
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
                                   XDG_DATA_HOME=str(root / "data"), XDG_RUNTIME_DIR=str(root / "runtime"),
                                   PATH=str(args.binary.parent) + os.pathsep + os.environ["PATH"])
                process = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                                            "--session", "--binary", str(args.binary), "--output", str(args.output)],
                                           env=environment, start_new_session=True)
                try:
                    if process.wait(timeout=75):
                        raise RuntimeError(f"Settings smoke test failed; see {args.output}")
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
