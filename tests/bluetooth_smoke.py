#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check Bluetooth panel discovery, popup bounds and grab release in an isolated X11 session."""
import argparse
import contextlib
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, visible_window, window_for_size, geometry, window_position


def session(args):
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        def toggle(offset=195):
            run("xdotool", "mousemove", str(round((int(1280 / args.scale) - offset) * args.scale)),
                str(round(16 * args.scale)), "click", "1")
        def popup():
            return visible_window("Midnight Bluetooth devices")
        try:
            until(lambda: window_for_size(1280, round(30 * args.scale)), "panel missing")
            second = subprocess.run([str(args.binary)], capture_output=True, timeout=5)
            assert second.returncode != 0, "second WM took over"
            toggle()
            until(popup, "Bluetooth popup missing")
            time.sleep(2)
            width, height = geometry(popup())
            x, y = window_position(popup())
            assert 0 <= x and 0 <= y and x + width <= 1280 and y + height <= 720
            run("import", "-window", "root", str(args.output / "bluetooth.png"))
            # Keyboard scanning must remain responsive even if hardware is unavailable.
            run("xdotool", "key", "Return", "Down", "Page_Down", "Up", "Page_Up", "Escape")
            until(lambda: not popup(), "Escape did not release Bluetooth grabs")
            toggle()
            until(popup, "Bluetooth did not reopen")
            toggle(167)
            until(lambda: not popup(), "network did not close Bluetooth")
            until(lambda: visible_window("Midnight network status"), "network missing")
            toggle()
            until(popup, "Bluetooth did not open from network")
            assert not visible_window("Midnight network status"), "Bluetooth left network open"
            run("xdotool", "mousemove", "700", "680", "click", "1")
            until(lambda: not popup(), "outside click did not close Bluetooth")
            assert wm.poll() is None, "WM exited during Bluetooth queries"
            print("Bluetooth popup, scan input, scrolling, bounds and popup grab handoff passed.")
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
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-bluetooth-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-bluetooth-") as tmp:
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
                    if process.wait(timeout=120):
                        raise RuntimeError("Bluetooth smoke failed; see " + str(args.output))
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
