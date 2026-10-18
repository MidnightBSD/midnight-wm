#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check folder totals/selection/refresh in isolated Xvfb using status-bar OCR.

Requires Xvfb, xdotool, ImageMagick and tesseract. Keeps screenshots and logs in
its printed temporary artifact directory; never uses the user's HOME/config.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, window_for_size, visible_window, window_position, geometry

REPO = Path(__file__).resolve().parents[1]


def session(root):
    folder = root / "files"
    folder.mkdir()
    (folder / "nested").mkdir()
    for name, size in (("a.bin", 1048576), ("b.bin", 524288), ("nested/c.bin", 524288)):
        with (folder / name).open("wb") as file:
            file.truncate(size)
    with (root / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(REPO / "build/midnight-wm")], stdout=log, stderr=subprocess.STDOUT)
        try:
            until(lambda: window_for_size(1280, 30), "panel missing")
            second = subprocess.run([str(REPO / "build/midnight-wm")], capture_output=True, timeout=5)
            assert second.returncode != 0, "second WM took over"
            until(lambda: "true" in run("gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                  "--object-path", "/org/freedesktop/DBus", "--method",
                  "org.freedesktop.DBus.NameHasOwner", "org.freedesktop.FileManager1"),
                  "FileManager1 service missing")
            run("gdbus", "call", "--session", "--timeout", "5", "--dest", "org.freedesktop.FileManager1",
                "--object-path", "/org/freedesktop/FileManager1", "--method",
                "org.freedesktop.FileManager1.ShowFolders", repr([folder.as_uri()]), "")
            window = until(lambda: window_for_size(620, 430), "folder missing")

            def status(name):
                run("import", "-window", window, str(root / (name + ".png")))
                width, height = geometry(window)
                run("convert", str(root / (name + ".png")), "-crop", f"{width}x22+0+{height-22}",
                    "-resize", "400%", str(root / "status.png"))
                return run("tesseract", str(root / "status.png"), "stdout", "--psm", "7",
                           stderr=subprocess.DEVNULL).strip()

            def expect(name, count, size, timeout=10):
                seen = []
                def matches():
                    text = status(name)
                    seen.append(text)
                    return f"{count}objects" in text.replace(" ", "") and size in text.replace(" ", "")
                try:
                    until(matches, f"status {name} did not reach {count} objects / {size}", timeout=timeout)
                except RuntimeError:
                    raise AssertionError(seen[-4:])
                (root / (name + ".txt")).write_text(seen[-1] + "\n")

            expect("total", 3, "2.0MiB")
            x, y = window_position(window)
            run("xdotool", "mousemove", str(x + 45), str(y + 95), "click", "1")
            expect("selected", 3, "1.0MiB")
            with (folder / "a.bin").open("r+b") as file:
                file.truncate(3145728)
            expect("selected-updated", 3, "3.0MiB")
            # Adding an earlier-sorted item must not silently change selection.
            (folder / "0-empty").touch()
            expect("selection-retained", 4, "3.0MiB")
            (folder / "a.bin").unlink()
            expect("selection-removed", 3, "1.0MiB")
            # No top-level event: idle periodic traversal must see nested changes.
            with (folder / "nested/c.bin").open("r+b") as file:
                file.truncate(1572864)
            expect("nested-update", 3, "2.0MiB", timeout=38)
            run("xdotool", "mousemove", str(x + 150), str(y + 95), "click", "1")
            expect("selected-kib", 3, "512KiB")
            run("xdotool", "mousemove", str(x + 430), str(y + 350), "click", "1")
            expect("deselected", 3, "2.0MiB")
            run("xdotool", "mousemove", "1000", "650")
            run("import", "-window", "root", str(root / "desktop.png"))
            run("xdotool", "mousemove", str(x + 600), str(y + 15), "click", "1")
            until(lambda: not visible_window("Folder: " + str(folder)), "folder did not close")
            print("PASS: total, IEC units, selection, live file size, selection retention/removal, nested refresh, deselection, close")
        finally:
            wm.terminate()
            wm.wait(timeout=10)


if "--session" in sys.argv:
    session(Path(sys.argv[-1]))
else:
    root = Path(tempfile.mkdtemp(prefix="midnight-content-size-smoke-"))
    print(f"Artifacts: {root}", flush=True)
    for name in ("home", "config/midnight-wm", "config-system", "data", "runtime"):
        (root / name).mkdir(parents=True)
    (root / "runtime").chmod(0o700)
    (root / "config/midnight-wm/config.ini").write_text(
        "[Workspace]\nui_scale=1.0\ndark_mode=" + ("true" if "--dark" in sys.argv else "false") + "\n")
    readfd, writefd = os.pipe()
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(writefd), "-screen", "0", "1280x720x24",
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
        child = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                                  "--session", str(root)], env=env, start_new_session=True)
        try:
            assert child.wait(timeout=100) == 0, f"smoke test failed; see {root}"
        finally:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    finally:
        xvfb.terminate()
        xvfb.wait(timeout=5)
