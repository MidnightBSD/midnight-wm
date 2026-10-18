#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Isolated folder minimize/restore, workspace and title-control checks."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, window_for_size, window_position, geometry, visible_window

REPO = Path(__file__).resolve().parents[1]


def session(root, scale):
    folder = root / "Folder minimize test"
    folder.mkdir()
    (folder / "example.txt").write_text("keep folder contents\n")
    with (root / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(REPO / "build/midnight-wm")], stdout=log, stderr=subprocess.STDOUT)
        try:
            until(lambda: window_for_size(1280, round(30 * scale)), "panel missing")
            second = subprocess.run([str(REPO / "build/midnight-wm")], capture_output=True, timeout=5)
            assert second.returncode != 0, "second WM took over"
            until(lambda: "true" in run("gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                  "--object-path", "/org/freedesktop/DBus", "--method",
                  "org.freedesktop.DBus.NameHasOwner", "org.freedesktop.FileManager1"), "FileManager1 missing")
            run("gdbus", "call", "--session", "--dest", "org.freedesktop.FileManager1",
                "--object-path", "/org/freedesktop/FileManager1", "--method",
                "org.freedesktop.FileManager1.ShowFolders", repr([folder.as_uri()]), "")
            title = "Folder: " + str(folder)
            window = until(lambda: visible_window(title), "folder missing")
            original = (*window_position(window), *geometry(window))

            def click(x, y, button=1, repeat=1):
                left, top = window_position(window)
                run("xdotool", "mousemove", str(left + round(x * scale)), str(top + round(y * scale)),
                    "click", "--repeat", str(repeat), "--delay", "100", str(button))

            def mapped():
                return "Map State: IsViewable" in run("xwininfo", "-id", window)

            def minimize():
                width = geometry(window)[0] / scale
                click(width - 44, 16)
                until(lambda: not mapped(), "minimize did not unmap folder")

            def restore():
                # First task button; the minimized folder must retain this entry.
                run("xdotool", "mousemove", str(round(250 * scale)), str(round(15 * scale)), "click", "1")
                until(mapped, "task button did not restore folder")
                assert visible_window(title) == window, "restore recreated the folder"

            for button in (2, 3):
                click(576, 16, button)
                time.sleep(.15)
                assert mapped() and geometry(window) == original[2:], "non-left minimize changed folder"
            run("import", "-window", window, str(root / "normal.png"))
            run("import", "-window", "root", str(root / "active.png"))
            pixel = f"%[pixel:p{{{round(202 * scale)},{round(16 * scale)}}}]"
            active_color = run("convert", str(root / "active.png"), "-format", pixel, "info:")
            minimize()
            def inactive_marker():
                run("import", "-window", "root", str(root / "minimized.png"))
                return run("convert", str(root / "minimized.png"), "-format", pixel, "info:") != active_color
            until(inactive_marker, "minimized task retained its active marker")
            run("xdotool", "set_desktop", "1")
            until(lambda: run("xdotool", "get_desktop").strip() == "1", "workspace did not switch")
            run("xdotool", "set_desktop", "0")
            until(lambda: run("xdotool", "get_desktop").strip() == "0", "workspace did not return")
            assert not mapped(), "workspace change restored minimized folder"
            for _ in range(2):
                run("xdotool", "key", "super+d")
                time.sleep(.2)
                assert not mapped(), "Show Desktop restored minimized folder"
            restore()
            assert (*window_position(window), *geometry(window)) == original, "normal geometry lost"
            # Shading still works, and explicit task activation expands the folder.
            click(150, 16, 2)
            until(lambda: geometry(window)[1] == round(34 * scale), "shade failed")
            minimize()
            restore()
            assert geometry(window) == original[2:], "shaded restore did not expand"
            # Minimize must preserve maximization and the geometry used to restore it.
            click(150, 16, repeat=2)
            until(lambda: geometry(window)[0] == 1280, "maximize failed")
            maximum = (*window_position(window), *geometry(window))
            minimize()
            restore()
            assert (*window_position(window), *geometry(window)) == maximum, "maximized geometry lost"
            time.sleep(.4)
            click(150, 16, repeat=2)
            until(lambda: geometry(window) == original[2:], "maximized restore lost original geometry")
            width = geometry(window)[0] / scale
            click(width - 20, 16)
            until(lambda: not visible_window(title), "Close stopped working")
            print("PASS: minimize, task restore, workspace/Show Desktop, shade, maximize, geometry, buttons, Close")
        finally:
            wm.terminate()
            wm.wait(timeout=10)


if "--session" in sys.argv:
    session(Path(sys.argv[-2]), float(sys.argv[-1]))
else:
    scale = 2.0 if "--scale2" in sys.argv else 1.0
    root = Path(tempfile.mkdtemp(prefix="midnight-folder-minimize-"))
    print(f"Artifacts: {root}", flush=True)
    for name in ("home", "config/midnight-wm", "config-system", "data", "runtime"):
        (root / name).mkdir(parents=True)
    (root / "runtime").chmod(0o700)
    (root / "config/midnight-wm/config.ini").write_text(
        f"[Workspace]\nui_scale={scale}\ndark_mode=" + ("true" if "--dark" in sys.argv else "false") + "\n")
    readfd, writefd = os.pipe()
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(writefd), "-screen", "0", "1280x1000x24",
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
                   XDG_RUNTIME_DIR=str(root / "runtime"))
        process = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                                    "--session", str(root), str(scale)], env=env, start_new_session=True)
        try:
            assert process.wait(timeout=60) == 0, f"see {root}"
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    finally:
        xvfb.terminate()
        xvfb.wait(timeout=5)
