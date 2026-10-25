#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Isolated real-locate, panel, GTK and default-application integration check."""
import contextlib
import os
from pathlib import Path
import signal
import subprocess
import sys
sys.dont_write_bytecode = True
import tempfile
import time
from settings_smoke import run, until, visible_window, window_position, window_for_size

repo = Path(__file__).resolve().parents[1]
temporary_context = (contextlib.nullcontext(os.environ["SEARCH_SMOKE_ROOT"])
                     if os.environ.get("SEARCH_SMOKE_SESSION") else
                     contextlib.nullcontext(tempfile.mkdtemp(prefix="midnight-search-smoke-")))
with temporary_context as temporary:
    root = Path(temporary)
    if not os.environ.get("SEARCH_SMOKE_SESSION"):
        print(f"Search test artifacts: {root}", flush=True)
    for name in ("home", "config", "config-system", "data/applications", "runtime", "files"):
        (root / name).mkdir(parents=True, exist_ok=True)
    (root / "runtime").chmod(0o700)
    paths = [root / "files" / f"spotlight-proof-{i:02}.txt" for i in range(25)]
    for path in paths:
        path.write_text("Search test\n")
    literal = root / "files/literal[?]*.txt"
    literal.write_text("Literal pattern test\n")
    # A right-to-left override would display this name as "disguise.txt"; the app
    # must still open the real path while showing a replacement character.
    disguised = root / "files/disguise.\u202etxt"
    disguised.write_text("Bidi override test\n")
    database = root / "locate.database"
    with database.open("wb") as output:
        subprocess.run(["/usr/libexec/locate.mklocatedb"],
                       input="".join(f"{p}\n" for p in [*paths, literal, disguised]).encode(), stdout=output, check=True)
    handler = root / "open-file"
    handler.write_text(f'#!/bin/sh\nprintf "%s\\n" "$1" >> "{root / "opened"}"\n')
    handler.chmod(0o700)
    (root / "data/applications/search-test.desktop").write_text(
        f"[Desktop Entry]\nType=Application\nName=Search Test\nExec={handler} %f\nMimeType=text/plain;\n")
    (root / "config/mimeapps.list").write_text("[Default Applications]\ntext/plain=search-test.desktop;\n")
    env = os.environ.copy()
    for key in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID"):
        env.pop(key, None)
    env.update(HOME=str(root / "home"), XDG_CONFIG_HOME=str(root / "config"),
               XDG_CONFIG_DIRS=str(root / "config-system"), XDG_DATA_HOME=str(root / "data"),
               XDG_RUNTIME_DIR=str(root / "runtime"), LOCATE_PATH=str(database))
    if not os.environ.get("SEARCH_SMOKE_SESSION"):
        # Re-enter the script inside the isolated display and bus; child owns fixtures.
        readfd, writefd = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(writefd), "-screen", "0", "1280x720x24",
                                 "-nolisten", "tcp"], pass_fds=(writefd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(writefd)
        try:
            with os.fdopen(readfd) as stream:
                env["DISPLAY"] = ":" + stream.readline().strip()
            env["SEARCH_SMOKE_SESSION"] = "1"
            env["SEARCH_SMOKE_ROOT"] = str(root)
            subprocess.run(["dbus-run-session", "--", "python3", str(Path(__file__).resolve())], env=env, check=True, timeout=60)
        finally:
            xvfb.terminate()
            xvfb.wait()
    else:
        os.environ.update(env)
        wm = subprocess.Popen([str(repo / "build/midnight-wm")], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            until(lambda: window_for_size(1280, 30), "panel missing")
            second = subprocess.run([str(repo / "build/midnight-wm")], env=env, capture_output=True, timeout=5)
            assert second.returncode != 0, "second WM took over"
            run("xdotool", "mousemove", "1160", "16", "click", "1")
            window = until(lambda: visible_window("Search Files"), "search window missing")
            run("xdotool", "type", "SPOTLIGHT-proof")
            run("xdotool", "key", "Return")
            time.sleep(1)
            run("import", "-window", window, str(root / "search.png"))
            x, y = window_position(window)
            run("xdotool", "mousemove", str(x + 110), str(y + 105), "click", "1")
            opened = root / "opened"
            until(lambda: opened.exists(), "default app did not open first result")
            assert opened.read_text().splitlines()[0] == str(paths[0]), opened.read_text()
            run("xdotool", "key", "ctrl+End", "Return")
            until(lambda: len(opened.read_text().splitlines()) >= 2, "keyboard result activation failed")
            assert opened.read_text().splitlines()[-1] == str(paths[19]), opened.read_text()
            run("xdotool", "mousemove", str(x + 110), str(y + 25), "click", "1", "key", "ctrl+a")
            run("xdotool", "type", "[?]*")
            run("xdotool", "key", "alt+f")
            time.sleep(1)
            run("xdotool", "mousemove", str(x + 110), str(y + 105), "click", "1")
            until(lambda: len(opened.read_text().splitlines()) >= 3, "Find/literal search failed")
            assert opened.read_text().splitlines()[-1] == str(literal), opened.read_text()
            run("xdotool", "mousemove", str(x + 110), str(y + 25), "click", "1", "key", "ctrl+a")
            run("xdotool", "type", "disguise.")
            run("xdotool", "key", "alt+f")
            time.sleep(1)
            run("xdotool", "mousemove", str(x + 110), str(y + 105), "click", "1")
            until(lambda: len(opened.read_text().splitlines()) >= 4, "bidi-override file name search failed")
            assert opened.read_text().splitlines()[-1] == str(disguised), opened.read_text()
            run("xdotool", "key", "alt+c")
            until(lambda: not visible_window("Search Files"), "Close did not dismiss search")
            print("PASS: panel, Enter search, case folding, 20-result limit, click/default app, keyboard activation, Find, literal glob characters, bidi-override names, Close, second-WM rejection")
        finally:
            wm.terminate()
            wm.wait(timeout=10)
