#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check folder views, sorting, hit testing and operations in isolated Xvfb.

Requires xdotool, ImageMagick and tesseract. --quick --size WIDTHxHEIGHT is
intended for the list-view display matrix. Artifacts are retained in /tmp.
"""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, visible_window, window_position, geometry, window_for_size
REPO = Path(__file__).resolve().parents[1]


def session(root, scale, quick):
    source = root / "Folder views"
    source.mkdir()
    (source / "Directory").mkdir()
    for name, size, modified in (("Alpha.png", 4096, 1000), ("beta.txt", 2048, 3000), ("gamma.txt", 1024, 2000)):
        with (source / name).open("wb") as stream:
            stream.truncate(size)
        os.utime(source / name, (modified, modified))
    with (root / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(REPO / "build/midnight-wm")], stdout=log, stderr=subprocess.STDOUT)
        try:
            until(lambda: "true" in run("gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                  "--object-path", "/org/freedesktop/DBus", "--method",
                  "org.freedesktop.DBus.NameHasOwner", "org.freedesktop.FileManager1"), "FileManager1 missing")
            second = subprocess.run([str(REPO / "build/midnight-wm")], capture_output=True, timeout=5)
            assert second.returncode != 0

            def open_folder(path):
                run("gdbus", "call", "--session", "--dest", "org.freedesktop.FileManager1",
                    "--object-path", "/org/freedesktop/FileManager1", "--method",
                    "org.freedesktop.FileManager1.ShowFolders", repr([path.as_uri()]), "")
                return until(lambda: visible_window("Folder: " + str(path)), "folder missing")

            window = open_folder(source)

            def click(x, y, button=1):
                left, top = window_position(window)
                run("xdotool", "mousemove", str(left + round(x * scale)), str(top + round(y * scale)), "click", str(button))

            def key(*keys):
                run("xdotool", "key", *keys)

            def capture(name):
                run("import", "-window", window, str(root / (name + ".png")))

            def ocr(name, x, y, width, height):
                capture(name)
                crop = f"{round(width*scale)}x{round(height*scale)}+{round(x*scale)}+{round(y*scale)}"
                run("convert", str(root / (name + ".png")), "-crop", crop, "-resize", "400%", str(root / "ocr.png"))
                return run("tesseract", str(root / "ocr.png"), "stdout", "--psm", "7", stderr=subprocess.DEVNULL).strip()

            def first_file(expected):
                seen = []
                def check():
                    seen.append(ocr("sorted", 36, 109, 185, 30))
                    return expected.lower() in seen[-1].lower().replace(" ", "")
                try:
                    until(check, f"expected first file {expected}")
                except RuntimeError:
                    raise AssertionError(seen[-4:])

            def footer(expected):
                width, height = geometry(window)
                until(lambda: expected in ocr("footer", width/scale-150, height/scale-22, 145, 20).replace(" ", ""),
                      f"selected size not {expected}")

            def menu(index):
                run("xdotool", "windowraise", window, "windowfocus", window)
                click(115, 40)
                popup = until(lambda: window_for_size(int(250*scale+.5), int(224*scale+.5)), "View menu missing")
                left, top = window_position(popup)
                run("xdotool", "mousemove", str(left + round(75*scale)),
                    str(top + round((4+index*24+12)*scale)), "click", "1")
                def dismissed():
                    result = subprocess.run(["xwininfo", "-id", popup], capture_output=True, text=True)
                    return result.returncode != 0 or "IsUnMapped" in result.stdout
                until(dismissed, "View menu selection did not complete")

            menu(2) # View -> List View
            first_file("Alpha.png")
            capture("list")
            if not quick:
                for index, expected in ((3, "Alpha.png"), (4, "gamma.txt"), (5, "beta.txt"),
                                        (6, "Alpha.png"), (7, "Alpha.png"), (8, "beta.txt")):
                    menu(index)
                    first_file(expected)
                click(95, 120) # beta.txt under Type sort
                footer("2.0KiB")
                menu(4)
                footer("2.0KiB") # sorting preserves selected identity
                key("ctrl+1")
                footer("2.0KiB")
                capture("grid-selected")
                key("ctrl+2")
                footer("2.0KiB")
                click(80, 65) # Name header switches to ascending, then descending
                first_file("Alpha.png")
                click(80, 65)
                first_file("gamma.txt")
                click(510, 65) # Modified column: newest, then oldest
                first_file("beta.txt")
                click(510, 65)
                first_file("Alpha.png")
                click(280, 65) # Size header
                first_file("Alpha.png")
                click(360, 65) # Type header
                first_file("beta.txt")
                click(80, 120, 3)
                time.sleep(.2)
                key("Escape")
                footer("2.0KiB")
                key("ctrl+c")
                destination = root / "Destination"
                destination.mkdir()
                source_window = window
                window = open_folder(destination)
                # New windows use grid independently of the source's list view.
                capture("independent-grid")
                key("ctrl+2", "ctrl+v")
                until(lambda: (destination / "beta.txt").exists(), "list selection copied wrong file")
                assert (destination / "beta.txt").stat().st_size == 2048
                key("ctrl+v")
                until(lambda: len(list(destination.iterdir())) == 2, "copy collision failed")
                click(80, 90)
                key("ctrl+x")
                target = root / "Move target"
                target.mkdir()
                window = open_folder(target)
                key("ctrl+2", "ctrl+v")
                until(lambda: len(list(target.iterdir())) == 1, "list move failed")
                until(lambda: len(list(destination.iterdir())) == 1, "move left source")
                # A missing source must fail without inventing a destination file.
                time.sleep(.5)
                click(80, 90)
                key("ctrl+c")
                moved = next(target.iterdir())
                moved.unlink()
                window = source_window
                run("xdotool", "windowraise", window, "windowfocus", window)
                before = {path.name for path in source.iterdir()}
                key("ctrl+v")
                time.sleep(.6)
                assert {path.name for path in source.iterdir()} == before, "failed copy created a file"
                # Dismiss any operation error popup before the scroll check.
                click(450, 350)
                run("xdotool", "windowfocus", window)
                # Ctrl-drag a list row onto the Directory row: XDND must use list hit testing.
                left, top = window_position(window)
                run("xdotool", "mousemove", str(left+round(80*scale)), str(top+round(120*scale)),
                    "keydown", "ctrl", "mousedown", "1")
                run("xdotool", "mousemove", str(left+round(100*scale)), str(top+round(127*scale)))
                time.sleep(.2)
                run("xdotool", "mousemove", str(left+round(100*scale)), str(top+round(90*scale)))
                time.sleep(.3)
                run("xdotool", "mouseup", "1", "keyup", "ctrl")
                until(lambda: (source / "Directory/beta.txt").exists(), "list row XDND destination failed")
                assert (source / "beta.txt").exists(), "Ctrl-drag moved instead of copying"

            for index in range(40):
                (source / f"z-fill-{index:02}.txt").write_text("x")
            time.sleep(.8)
            # Sorting A-Z makes the last row predictable; Down must scroll to it.
            menu(3)
            first_file("Alpha.png")
            click(400, 350) # Focus the view; Home resets scrolling before selecting its first row
            key("Home")
            click(80, 90)
            key(*(["Down"] * 43))
            capture("scrolled")
            until(lambda: "z-fill-39.txt" in ocr("scrolled", 36, 349, 185, 30).replace(" ", ""),
                  "keyboard selection did not scroll to the final row")
            key("Home")
            until(lambda: "Directory" in ocr("top", 36, 79, 185, 30), "Home did not return to first row")
            capture("list-populated")
            run("xdotool", "mousemove", "10", "10")
            run("import", "-window", "root", str(root / "desktop-list.png"))
            if quick and scale == 2.0:
                # Xvfb lacks a smaller RandR mode: exercise root reflow with the
                # same test-only synthetic event as the resolution-check skill.
                with (root / "randr-resize.log").open("w") as resize_log:
                    resized_mode = subprocess.run(["xrandr", "--fb", "1280x720"], stdout=resize_log, stderr=subprocess.STDOUT)
                if resized_mode.returncode:
                    run(str(REPO / "build/screen_resize_event"), "1280", "720")
                def resized():
                    (root / "live-resize-tree.txt").write_text(run("xwininfo", "-root", "-tree"))
                    return geometry(window)[0] == 620
                until(resized, "list did not reflow after root resize")
                scale = 1.0
                capture("live-resize-list")
            # Real pointer resize: the model and hit testing must follow narrow layouts.
            left, top = window_position(window)
            width, height = geometry(window)
            run("xdotool", "mousemove", str(left+width-round(8*scale)), str(top+height-round(8*scale)),
                "mousedown", "1", "mousemove", str(left+round(292*scale)), str(top+round(252*scale)), "mouseup", "1")
            until(lambda: geometry(window)[0] <= round(305*scale), "narrow resize failed")
            capture("narrow-list")
            click(80, 90)
            key("Return") # Directory row opens through the same row hit test
            until(lambda: visible_window("Folder: " + str(source / "Directory")), "list folder activation failed")
            print("PASS: grid/list, sorting, headers, selection, scrolling, resize, row activation" +
                  ("; copy/collision/move/failure/XDND" if not quick else ""))
        finally:
            wm.terminate()
            wm.wait(timeout=10)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--session", action="store_true")
parser.add_argument("--root", type=Path)
parser.add_argument("--size", default="1280x720")
parser.add_argument("--dark", action="store_true")
parser.add_argument("--quick", action="store_true")
args = parser.parse_args()
width, height = map(int, args.size.split("x"))
scale = 2.0 if height >= 1800 else 1.25 if height >= 1300 else 1.0
if args.session:
    session(args.root, scale, args.quick)
else:
    root = Path(tempfile.mkdtemp(prefix="midnight-folder-views-smoke-"))
    print(f"Artifacts: {root}", flush=True)
    for name in ("home", "config/midnight-wm", "config-system", "data", "runtime"):
        (root / name).mkdir(parents=True)
    (root / "runtime").chmod(0o700)
    (root / "config/midnight-wm/config.ini").write_text(
        "[Workspace]\nui_scale=0\ndark_mode=" + ("true" if args.dark else "false") + "\n")
    readfd, writefd = os.pipe()
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(writefd), "-screen", "0", f"{width}x{height}x24",
                             "-nolisten", "tcp"], pass_fds=(writefd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(writefd)
    try:
        with os.fdopen(readfd) as stream:
            display = stream.readline().strip()
        env = os.environ.copy()
        for key in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID"):
            env.pop(key, None)
        env.update(DISPLAY=":"+display, HOME=str(root / "home"), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_CONFIG_DIRS=str(root / "config-system"), XDG_DATA_HOME=str(root / "data"),
                   XDG_RUNTIME_DIR=str(root / "runtime"), LC_ALL="C", MIDNIGHT_WM_TEST_SYNTHETIC_RESIZE="1")
        command = ["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()), "--session",
                   "--root", str(root), "--size", args.size]
        if args.quick:
            command.append("--quick")
        child = subprocess.Popen(command, env=env, start_new_session=True)
        try:
            assert child.wait(timeout=120) == 0, f"see {root}"
        finally:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    finally:
        xvfb.terminate()
        xvfb.wait(timeout=5)
