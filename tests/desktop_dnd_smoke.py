#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check desktop file drag and drop in an isolated X11 session."""
import argparse
import contextlib
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, visible_window, window_for_size


def session(args):
    desktop = Path.home() / "Desktop"
    desktop.mkdir(exist_ok=True)
    (desktop / "A folder").mkdir()
    (desktop / "A folder/keep.txt").write_text("recoverable contents")
    (desktop / "B destination").mkdir()
    (desktop / "C file.txt").write_text("copy contents")
    data = Path(os.environ["XDG_DATA_HOME"])
    database = data / "midnight-wm/workspace.db"
    database.parent.mkdir(parents=True, exist_ok=True)
    custom_icon = data / "saved-folder.svg"
    custom_icon.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40"><path fill="#f800f8" d="M0 0h40v40H0z"/></svg>')
    with sqlite3.connect(database) as db:
        db.execute("CREATE TABLE objects (id TEXT PRIMARY KEY, kind INTEGER NOT NULL, title TEXT NOT NULL, target TEXT NOT NULL, icon TEXT NOT NULL, parent TEXT NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL)")
        for ident, kind, name, icon in (("saved-folder", 1, "A folder", str(custom_icon)),):
            db.execute("INSERT INTO objects VALUES (?, ?, ?, ?, ?, '', 0, 0)",
                       (ident, kind, name, str(desktop / name), icon))
    def has_saved_icon():
        snapshot = args.output / "saved-icon.png"
        run("import", "-window", "root", str(snapshot))
        histogram = run("convert", str(snapshot), "-format", "%c", "histogram:info:-")
        return "#F800F8" in histogram.upper()
    def point(name):
        # Defaults and physical entries are sorted by title, top to bottom.
        names = sorted(["Documents", "Downloads", "Home", "Pictures", "Templates", "Trash"] +
                       [p.name for p in desktop.iterdir()])
        index = names.index(name)
        rows = max(1, int((720 / args.scale - 30 - 13) / 90))
        return round((42 + (index // rows) * 90) * args.scale), round((68 + (index % rows) * 90) * args.scale)
    def drag(name, destination, copy=False):
        x, y = point(name)
        tx, ty = point(destination)
        if copy:
            run("xdotool", "keydown", "ctrl")
        try:
            run("xdotool", "mousemove", str(x), str(y), "mousedown", "1")
            run("xdotool", "mousemove", str(x + round(12 * args.scale)), str(y + round(8 * args.scale)))
            time.sleep(.2)
            if name != "Home":
                preview = until(lambda: visible_window("Midnight drag preview"), "drag preview missing")
                run("import", "-window", "root", str(args.output / "drag-preview.png"))
            run("xdotool", "mousemove", str(tx), str(ty))
            time.sleep(.3)
            run("xdotool", "mouseup", "1")
            time.sleep(.5)
            until(lambda: visible_window("Midnight drag preview") is None, "preview remained after release")
        finally:
            if copy:
                run("xdotool", "keyup", "ctrl")
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        try:
            until(lambda: window_for_size(1280, round(30 * args.scale)), "panel missing")
            second = subprocess.run([str(args.binary)], capture_output=True, timeout=5)
            assert second.returncode != 0, "second WM took over"
            until(has_saved_icon, "saved folder icon not drawn before drag")
            drag("C file.txt", "B destination", copy=True)
            until(lambda: (desktop / "B destination/C file.txt").exists(), "copy onto folder failed")
            assert (desktop / "C file.txt").exists(), "copy removed source"
            drag("C file.txt", "B destination", copy=True)
            until(lambda: (desktop / "B destination/C file 2.txt").exists(), "collision copy failed")
            drag("B destination", "B destination")
            assert not (desktop / "B destination/B destination").exists(), "self drop recursed"
            drag("C file.txt", "B destination")
            until(lambda: not (desktop / "C file.txt").exists(), "move onto folder failed")
            assert (desktop / "B destination/C file 3.txt").read_text() == "copy contents"
            drag("Home", "Trash")
            assert Path.home().is_dir() and desktop.is_dir(), "built-in shortcut moved its target"
            drag("A folder", "Trash")
            if (desktop / "A folder").exists():
                # MidnightBSD /tmp mounts can lack GIO Trash support. A failed
                # operation must preserve both the directory and its contents.
                assert (desktop / "A folder/keep.txt").read_text() == "recoverable contents"
                raise RuntimeError("Trash drop failed; source preserved (check GIO support in wm.log)")
            trash = Path(os.environ["XDG_DATA_HOME"]) / "Trash"
            until(lambda: any(trash.glob("files/*/keep.txt")), "trashed folder contents missing")
            assert any(p.read_text() == "recoverable contents" for p in trash.glob("files/*/keep.txt")), "trashed contents changed"
            assert any("A%20folder" in p.read_text() or "A folder" in p.read_text()
                       for p in trash.glob("info/*.trashinfo")), "restore metadata missing"
            until(lambda: not has_saved_icon(), "trashed folder retained its desktop icon")
            run("import", "-window", "root", str(args.output / "desktop-after-drop.png"))
            assert wm.poll() is None, "WM exited during drops"
            print("Desktop copy, collision, move, self-drop rejection and recoverable Trash passed.")
        finally:
            if wm.poll() is None:
                wm.terminate()
                wm.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/midnight-wm"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("--session", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    if args.session:
        session(args)
        return
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-desktop-dnd-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-desktop-dnd-") as tmp:
        root = Path(tmp)
        for path in ("home", "config/midnight-wm", "config-system", "data", "runtime", "bin"):
            (root / path).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        (root / "config/midnight-wm/config.ini").write_text(f"[Workspace]\nui_scale={args.scale}\nshow_battery=false\ncompositing=false\n")
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
                                            "--session", "--binary", str(args.binary), "--output", str(args.output), "--scale", str(args.scale)],
                                           env=env, start_new_session=True)
                try:
                    if process.wait(timeout=60):
                        raise RuntimeError("Desktop drag smoke failed; see " + str(args.output))
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
