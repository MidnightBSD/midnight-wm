#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Isolated System Monitor check: process rows, End Process, Force Quit, Hardware tab."""
import contextlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True
import tempfile
import time
from settings_smoke import run, until, visible_window, window_position

repo = Path(__file__).resolve().parents[1]


def app_windows(pid):
    result = subprocess.run(["xdotool", "search", "--onlyvisible", "--pid", str(pid)],
                            capture_output=True, text=True, timeout=5)
    return result.stdout.split()


def end_process(monitor, victim, name, mnemonic):
    """Select a process through type-ahead search, then confirm the chosen action."""
    x, y = window_position(monitor)
    run("xdotool", "mousemove", str(x + 300), str(y + 140), "click", "1")
    time.sleep(0.3)
    run("xdotool", "type", name)
    time.sleep(0.5)
    run("xdotool", "key", "Escape")
    time.sleep(0.3)
    before = len(app_windows(app.pid))
    run("xdotool", "key", mnemonic)
    until(lambda: len(app_windows(app.pid)) > before, f"{mnemonic} did not open a confirmation")
    time.sleep(0.3)
    assert victim.poll() is None, "process ended before confirmation"
    run("xdotool", "key", mnemonic)
    until(lambda: victim.poll() is not None, f"{name} survived {mnemonic}")
    until(lambda: len(app_windows(app.pid)) == before, "confirmation did not close")


temporary_context = (contextlib.nullcontext(os.environ["MONITOR_SMOKE_ROOT"])
                     if os.environ.get("MONITOR_SMOKE_SESSION") else
                     contextlib.nullcontext(tempfile.mkdtemp(prefix="midnight-monitor-smoke-")))
with temporary_context as temporary:
    root = Path(temporary)
    if not os.environ.get("MONITOR_SMOKE_SESSION"):
        print(f"System Monitor test artifacts: {root}", flush=True)
    for name in ("home", "config", "config-system", "data", "runtime", "bin"):
        (root / name).mkdir(parents=True, exist_ok=True)
    (root / "runtime").chmod(0o700)
    env = os.environ.copy()
    for key in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID"):
        env.pop(key, None)
    env.update(HOME=str(root / "home"), XDG_CONFIG_HOME=str(root / "config"),
               XDG_CONFIG_DIRS=str(root / "config-system"), XDG_DATA_HOME=str(root / "data"),
               XDG_RUNTIME_DIR=str(root / "runtime"))
    if not os.environ.get("MONITOR_SMOKE_SESSION"):
        readfd, writefd = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(writefd), "-screen", "0", "1280x720x24",
                                 "-nolisten", "tcp"], pass_fds=(writefd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(writefd)
        try:
            with os.fdopen(readfd) as stream:
                env["DISPLAY"] = ":" + stream.readline().strip()
            env["MONITOR_SMOKE_SESSION"] = "1"
            env["MONITOR_SMOKE_ROOT"] = str(root)
            subprocess.run(["dbus-run-session", "--", "python3", str(Path(__file__).resolve())], env=env, check=True, timeout=90)
        finally:
            xvfb.terminate()
            xvfb.wait()
    else:
        os.environ.update(env)
        # Victims are copies of sleep(1) with distinctive names. The second one
        # carries a right-to-left override and a control byte, which the
        # monitor must neutralize while still listing and ending the process.
        plain = root / "bin/monitorvictim"
        spoofed = root / ("bin/monitorspoof‮\x01")
        for path in (plain, spoofed):
            shutil.copy("/bin/sleep", path)
            path.chmod(0o700)
        victims = [subprocess.Popen([str(path), "1000"], env=env) for path in (plain, spoofed)]
        wm = subprocess.Popen([str(repo / "build/midnight-wm")], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        app = subprocess.Popen([str(repo / "build/midnightbsd-system-monitor")], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            monitor = until(lambda: visible_window("System Monitor"), "monitor window missing")
            run("xdotool", "windowactivate", "--sync", monitor)
            time.sleep(2.5)  # first process sample and a refresh so rows exist
            run("import", "-window", "root", str(root / "processes.png"))
            end_process(monitor, victims[0], "monitorvictim", "alt+e")
            end_process(monitor, victims[1], "monitorspoof", "alt+f")
            assert app.poll() is None, "monitor exited"
            # Cancel path: the default response must leave the process alone.
            survivor = subprocess.Popen([str(plain), "1000"], env=env)
            try:
                time.sleep(2.5)
                x, y = window_position(monitor)
                run("xdotool", "mousemove", str(x + 300), str(y + 140), "click", "1")
                run("xdotool", "type", "monitorvictim")
                run("xdotool", "key", "Escape")
                before = len(app_windows(app.pid))
                run("xdotool", "key", "alt+e")
                until(lambda: len(app_windows(app.pid)) > before, "End Process did not confirm")
                run("xdotool", "key", "Return")
                until(lambda: len(app_windows(app.pid)) == before, "Cancel did not close the dialog")
                time.sleep(0.5)
                assert survivor.poll() is None, "Cancel ended the process"
            finally:
                survivor.kill()
                survivor.wait()
            # Hardware tab: the drive list must reserve room for several rows.
            run("xdotool", "mousemove", str(x + 370), str(y + 27), "click", "1")
            time.sleep(1.5)
            run("import", "-window", "root", str(root / "hardware.png"))
            print("PASS: process rows, type-ahead selection, End Process, Force Quit on a bidi-override name, Cancel, Hardware tab")
        finally:
            for victim in victims:
                victim.kill()
                victim.wait()
            app.terminate()
            app.wait(timeout=10)
            wm.terminate()
            wm.wait(timeout=10)
