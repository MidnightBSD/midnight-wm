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


def atomic_write(path, text):
    temporary = path.with_suffix(".new")
    temporary.write_text(text)
    temporary.replace(path)


def session(args):
    output = args.output
    config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
    folder = Path(os.environ["HOME"]) / "Test folder"
    folder.mkdir()
    (folder / "kept.txt").write_text("The folder must stay open during reload.\n")
    with (output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        try:
            panel = until(lambda: window_for_size(1280, 30), "initial panel did not appear")
            run("gdbus", "call", "--session", "--timeout", "5", "--dest",
                "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1",
                "--method", "org.freedesktop.FileManager1.ShowFolders", repr([folder.as_uri()]), "")
            folder_window = until(lambda: window_for_size(620, 430), "folder did not open")
            # The seventh Midnight menu row is Settings. PATH points to the build directory.
            run("xdotool", "mousemove", "--sync", "12", "12", "click", "1")
            time.sleep(0.2)
            run("xdotool", "mousemove", "--sync", "100", "190", "click", "1")
            settings = until(lambda: settings_window(), "Settings did not launch from Midnight menu")
            until(lambda: frame_extents(settings) == (5, 5, 28, 5),
                  "Settings did not receive initial frame extents")
            run("import", "-window", "root", str(output / "settings.png"))
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
            changed = ("[Workspace]\npanel_label=Smoke test\nui_scale=1.25\n"
                       "panel_color=#123456\naccent_color=#654321\ndesktop_color=#345678\n"
                       "[Unrelated]\nretained=keep this value\n")
            atomic_write(config, changed)
            until(lambda: geometry(panel) == (1280, 38), "live scale did not resize the existing panel")
            until(lambda: geometry(folder_window) == (775, 538), "live scale did not resize the existing folder")
            until(lambda: frame_extents(settings) == (6, 6, 35, 6),
                  "live scale did not update the existing Settings client's frame extents")
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
            print("Settings menu launch, Apply, preservation, live color/scale, and malformed-config recovery passed.")
        finally:
            wm.terminate()
            try:
                wm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                wm.kill()
                wm.wait(timeout=5)


def settings_window():
    result = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^Midnight Workspace Settings$"],
                            capture_output=True, text=True, timeout=5)
    return result.stdout.splitlines()[0] if result.returncode == 0 and result.stdout.strip() else None


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
