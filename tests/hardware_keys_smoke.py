#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check density scaling, tray placement, and hardware keys in a disposable session.

At 2256x1504 with the 1.5 scale that a 13.5 inch laptop panel selects, a
status notifier item must sit against the search button, and the volume and
brightness keys must show their indicator. Xvfb's RandR monitor always reports
a 96 DPI size, so the automatic choice itself is covered by display_scale_test.
The brightness key is pointed at a missing device so the real panel is never
dimmed; mixer levels are restored afterwards.
"""

import argparse
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

from settings_smoke import atomic_write, run, until, window_for_size

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT, SCALE = 2256, 1504, 1.5
OSD = (360, 81)  # 240x54 logical


def item():
    """Export one StatusNotifierItem until terminated."""
    import gi
    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    node = Gio.DBusNodeInfo.new_for_xml(
        '<node><interface name="org.kde.StatusNotifierItem">'
        '<property name="Id" type="s" access="read"/>'
        '<property name="Category" type="s" access="read"/>'
        '<property name="Status" type="s" access="read"/>'
        '<property name="IconName" type="s" access="read"/>'
        '<property name="Title" type="s" access="read"/>'
        '</interface></node>')
    values = {"Id": "smoke", "Category": "ApplicationStatus", "Status": "Active",
              "IconName": "input-keyboard", "Title": "Smoke"}
    bus.register_object("/StatusNotifierItem", node.interfaces[0], None,
                        lambda *a: GLib.Variant("s", values[a[4]]), None)
    bus.call_sync("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                  "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                  GLib.Variant("(s)", ("/StatusNotifierItem",)), None,
                  Gio.DBusCallFlags.NONE, 5000, None)
    GLib.MainLoop().run()


def children(window):
    """Direct children of a window as (width, height, x, y)."""
    tree = run("xwininfo", "-id", window, "-children")
    return [tuple(map(int, match)) for match in
            re.findall(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)\s+\+-?\d+\+-?\d+", tree)]


def session(args):
    config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        provider = None
        saved = None
        try:
            until(lambda: "_NET_SUPPORTING_WM_CHECK(WINDOW)" in
                  run("xprop", "-root", "_NET_SUPPORTING_WM_CHECK"), "window manager did not start")
            panel_height = round(30 * SCALE)
            panel = until(lambda: window_for_size(WIDTH, panel_height),
                          f"scale {SCALE} did not produce a {panel_height}px panel")

            provider = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--item"])
            # Logical layout: clock at width-4-87, search 32 before it.
            search_edge = round((int(WIDTH / SCALE) - 4 - 87 - 32) * SCALE)
            slot = round(4 * SCALE) + round(26 * SCALE)

            def status_window():
                return next((child for child in children(panel)
                             if child[0] == slot and child[1] == round(22 * SCALE)), None)
            found = until(status_window, "status notifier item did not appear in the panel")
            run("import", "-window", "root", "-crop", f"{WIDTH}x{panel_height}+0+0",
                str(args.output / "panel.png"))
            assert abs(found[2] + found[0] - search_edge) <= 1, \
                f"status item at x={found[2]} is not against the search button at {search_edge}"

            def osd():
                return window_for_size(*OSD)

            def expect_osd(key, message):
                run("xdotool", "key", key)
                until(osd, message, timeout=3)
                run("import", "-window", "root", str(args.output / f"osd-{key}.png"))
                until(lambda: not osd(), f"{key} indicator did not expire", timeout=5)

            if shutil.which("mixer"):
                level = subprocess.run(["mixer", "-o", "vol.volume"], capture_output=True, text=True)
                if level.returncode == 0 and level.stdout.startswith("vol.volume="):
                    saved = level.stdout.strip()
            expect_osd("XF86AudioRaiseVolume", "volume key did not show its indicator")
            if saved and not saved.endswith("=1.00:1.00"):
                assert run("mixer", "-o", "vol.volume").strip() != saved, \
                    "volume key did not change the mixer"
            expect_osd("XF86MonBrightnessUp", "brightness key did not show its indicator")

            atomic_write(config, f"[Workspace]\nui_scale={SCALE}\nmedia_keys=false\n[Session]\nstart_xscreensaver=false\n")
            time.sleep(2.5)  # the WM polls once per second and waits for a stable snapshot
            run("xdotool", "key", "XF86MonBrightnessUp")
            time.sleep(0.7)
            assert not osd(), "hardware keys stayed grabbed after media_keys=false"
        finally:
            if saved:
                run("mixer", saved)
            for process in (provider, wm):
                if process:
                    process.terminate()
                    with contextlib.suppress(subprocess.TimeoutExpired):
                        process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/midnight-wm")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--session", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--item", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.item:
        item()
        return
    args.binary = args.binary.resolve()
    if args.session:
        session(args)
        return
    for command in ("Xvfb", "dbus-run-session", "xdotool", "xwininfo", "xprop", "import"):
        if not shutil.which(command):
            parser.error(f"required command not found: {command}")
    if not args.binary.is_file():
        parser.error(f"build executable first: {args.binary}")
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-keys-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-keys-") as temporary:
        root = Path(temporary)
        for directory in ("home", "config/midnight-wm", "config-system", "data", "data-system", "runtime"):
            (root / directory).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        (root / "config/midnight-wm/config.ini").write_text(
            f"[Workspace]\nui_scale={SCALE}\n[Session]\nstart_xscreensaver=false\n")
        read_fd, write_fd = os.pipe()
        with (args.output / "xvfb.log").open("w") as log:
            xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                                     f"{WIDTH}x{HEIGHT}x24", "-nolisten", "tcp"],
                                    pass_fds=(write_fd,), stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True)
            os.close(write_fd)
            try:
                with os.fdopen(read_fd) as stream:
                    display = stream.readline().strip()
                if not display:
                    raise RuntimeError("Xvfb failed to start")
                environment = os.environ.copy()
                for variable in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK",
                                 "SSH_AGENT_PID", "XAUTHORITY", "SESSION_MANAGER"):
                    environment.pop(variable, None)
                environment.update(DISPLAY=f":{display}", HOME=str(root / "home"),
                                   XDG_CONFIG_HOME=str(root / "config"),
                                   XDG_CONFIG_DIRS=str(root / "config-system"),
                                   XDG_DATA_HOME=str(root / "data"), XDG_RUNTIME_DIR=str(root / "runtime"),
                                   MIDNIGHT_WM_TEST_BACKLIGHT=str(root / "no-backlight"),
                                   MIDNIGHT_KEEP_SESSION_PROCESSES="0", LC_ALL="C")
                process = subprocess.Popen(["dbus-run-session", "--", sys.executable,
                                            str(Path(__file__).resolve()), "--session",
                                            "--binary", str(args.binary), "--output", str(args.output)],
                                           env=environment, start_new_session=True)
                try:
                    if process.wait(timeout=90):
                        raise RuntimeError(f"Hardware key smoke test failed; see {args.output}")
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)
    print("hardware keys smoke passed")


if __name__ == "__main__":
    main()
