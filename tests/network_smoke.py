#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check network panel status and open-only details in an isolated X11 session."""
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
    fixture = args.output / "network-fixture.so"
    subprocess.run(["/usr/bin/c++", "-std=c++23", "-shared", "-fPIC",
                    str(Path(__file__).with_name("network_fixture.cpp")), "-o", str(fixture)], check=True)
    data = args.output / "network.txt"
    queries = args.output / "network-queries.log"
    queries.write_text("")
    def state(mode, rx=123456, tx=654321):
        temporary = data.with_suffix(".new")
        temporary.write_text(f"{mode} {rx} {tx}\n")
        temporary.replace(data)
    def count(event):
        return queries.read_text().splitlines().count(event)
    def capture(name):
        path = args.output / (name + ".png")
        run("import", "-window", "root", str(path))
        return path
    env = os.environ.copy()
    env.update(LD_PRELOAD=str(fixture), MIDNIGHT_NETWORK_FIXTURE=str(data), MIDNIGHT_NETWORK_LOG=str(queries))
    state(2)
    native = subprocess.check_output([str(args.binary.parent / "network_test")], env=env, text=True)
    assert "SSID=Midnight Test standard=802.11n (Wi-Fi 4) speed=540 RX=123456 TX=654321" in native, native
    assert "IP=192.0.2.11" in native, native
    state(4)
    aggregate = subprocess.check_output([str(args.binary.parent / "network_test")], env=env, text=True)
    assert "Native kind: 2" in aggregate and "laggtest0 SSID=Midnight Test" in aggregate, aggregate
    state(2)
    queries.write_text("")
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], env=env, stdout=log, stderr=subprocess.STDOUT)
        def toggle():
            x = round((int(1280 / args.scale) - 135) * args.scale)
            run("xdotool", "mousemove", str(x), str(round(16 * args.scale)), "click", "1")
        def popup():
            return visible_window("Midnight network status")
        try:
            until(lambda: window_for_size(1280, round(30 * args.scale)), "panel missing")
            second = subprocess.run([str(args.binary)], capture_output=True, timeout=5)
            assert second.returncode != 0, "second WM took over"
            time.sleep(3)
            assert count("details") == 0, "Wi-Fi details were read with the popup closed"
            toggle()
            until(popup, "network popup missing")
            until(lambda: count("details") >= 2, "details did not refresh while open")
            capture("wifi-details")
            state(2, 987654321, 123456789)
            before = count("details")
            until(lambda: count("details") > before, "live detail refresh stopped")
            capture("wifi-updated")
            run("xdotool", "key", "Escape")
            until(lambda: not popup(), "Escape did not close network popup")
            before = count("details")
            status_before = count("status")
            state(1)
            # Real 30-second status cadence, with detail queries remaining idle.
            until(lambda: count("status") > status_before, "closed panel status was not refreshed", timeout=35)
            assert count("details") == before, "details continued polling while closed"
            capture("wired-status")
            toggle()
            until(popup, "wired popup missing")
            capture("wired-details")
            run("xdotool", "mousemove", "700", "680", "click", "1")
            until(lambda: not popup(), "outside click did not release popup grabs")
            state(0)
            toggle()
            until(popup, "disconnected popup missing")
            capture("disconnected")
            run("xdotool", "key", "Escape")
            state(3)
            toggle()
            until(popup, "multiple-interface popup missing")
            window = popup()
            width, height = geometry(window)
            x, y = window_position(window)
            assert 0 <= x and 0 <= y and x + width <= 1280 and y + height <= 720, (x, y, width, height)
            run("xdotool", "key", "Down", "Page_Down", "Up", "Page_Up")
            capture("multiple-interfaces")
            # Audio and network popups must hand off grabs cleanly.
            volume_x = round((int(1280 / args.scale) - 107) * args.scale)
            run("xdotool", "mousemove", str(volume_x), str(round(16 * args.scale)), "click", "1")
            until(lambda: not popup(), "audio did not close network popup")
            until(lambda: window_for_size(round(260 * args.scale), round(230 * args.scale)), "audio popup did not open")
            toggle()
            until(popup, "network did not reopen from audio popup")
            assert not window_for_size(round(260 * args.scale), round(230 * args.scale)), "network left audio popup open"
            run("xdotool", "key", "Escape")
            assert wm.poll() is None, "WM exited during network updates"
            print("Network states, native Wi-Fi details, 30-second status polling, open-only details, popup bounds and grab release passed.")
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
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-network-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-network-") as tmp:
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
                        raise RuntimeError("Network smoke failed; see " + str(args.output))
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
