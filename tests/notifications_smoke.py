#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise notification actions, hints, replacement and close reasons in isolated Xvfb/D-Bus."""
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
sys.dont_write_bytecode = True
from settings_smoke import run, until, window_for_size, window_position

SERVICE = "org.freedesktop.Notifications"

def call(method, *arguments):
    return run("gdbus", "call", "--session", "--dest", SERVICE, "--object-path",
               "/org/freedesktop/Notifications", "--method", SERVICE + "." + method, *arguments)

def notify(actions="[]", hints="{}", timeout="0", replaces="0"):
    reply = call("Notify", "test", replaces, "", "Test notification", "Body", actions, hints, timeout)
    return re.search(r"uint32 (\d+)", reply).group(1)

def session(args):
    with (args.output / "wm.log").open("w") as log, (args.output / "signals.log").open("w") as signals:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        monitor = None
        try:
            def ready():
                try:
                    run("gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                        "--object-path", "/org/freedesktop/DBus", "--method",
                        "org.freedesktop.DBus.GetNameOwner", SERVICE)
                    return call("GetCapabilities")
                except subprocess.CalledProcessError:
                    return None
            caps = until(ready, "notification service unavailable")
            assert "actions" in caps and "icon-static" in caps and "persistence" not in caps
            other = subprocess.run([str(args.binary)], capture_output=True, text=True, timeout=5)
            assert other.returncode != 0, "a second WM took over"
            monitor = subprocess.Popen(["gdbus", "monitor", "--session", "--dest", SERVICE,
                                        "--object-path", "/org/freedesktop/Notifications"],
                                       stdout=signals, stderr=subprocess.STDOUT)
            time.sleep(.3)
            def seen(fragment):
                return fragment in (args.output / "signals.log").read_text()
            def signal_seen(name, ident, value):
                until(lambda: seen(f"{name} (uint32 {ident}, {value})"), f"missing {name} {ident} {value}")
            ident = notify("['open', 'Open file']")
            window = until(lambda: window_for_size(330, 118), "action notification missing")
            x, y = window_position(window)
            run("xdotool", "mousemove", str(x + 60), str(y + 102), "click", "1")
            signal_seen("ActionInvoked", ident, "'open'")
            signal_seen("NotificationClosed", ident, "uint32 2")
            until(lambda: not window_for_size(330, 118), "action notification did not close")

            ident = notify("['default', 'Open']", "{'resident': <true>}")
            window = until(lambda: window_for_size(330, 90), "resident notification missing")
            x, y = window_position(window)
            run("xdotool", "mousemove", str(x + 90), str(y + 40), "click", "1")
            signal_seen("ActionInvoked", ident, "'default'")
            assert window_for_size(330, 90) == window, "resident action dismissed notification"
            assert notify(hints="{'urgency': <byte 2>}", timeout="100", replaces=ident) == ident
            time.sleep(.4)
            assert window_for_size(330, 90) == window, "critical notification expired or replacement remapped"
            run("import", "-window", window, str(args.output / "critical.png"))
            call("CloseNotification", ident)
            signal_seen("NotificationClosed", ident, "uint32 3")

            ident = notify(timeout="100")
            signal_seen("NotificationClosed", ident, "uint32 1")
            # Malformed image length must be ignored without reading past the byte array.
            ident = notify(hints="{'image-data': <(2, 2, 8, true, 8, 4, [byte 255])>}")
            call("CloseNotification", ident)
            ident = notify(hints="{'image-data': <(1, 1, 3, false, 8, 3, [byte 255, 0, 0])>}")
            window = until(lambda: window_for_size(330, 90), "image notification missing")
            time.sleep(.2)
            run("import", "-window", window, str(args.output / "image.png"))
            pixel = run("convert", str(args.output / "image.png"), "-crop", "1x1+11+20", "txt:-")
            assert "#FF0000" in pixel, f"raw image was not rendered: {pixel}"
            call("CloseNotification", ident)
            image_uri = (args.output / "image.png").as_uri()
            ident = notify(hints="{'image-path': <'" + image_uri + "'>}")
            until(lambda: window_for_size(330, 90), "image-path notification missing")
            call("CloseNotification", ident)
            print("Notification actions, resident/default actions, urgency, expiry, replacement, close reasons, "
                  "image data/path, malformed image bounds, and second-WM rejection passed.")
        finally:
            if monitor:
                monitor.terminate()
                monitor.wait(timeout=5)
            wm.terminate()
            try:
                wm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                wm.kill()
                wm.wait(timeout=5)


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
    for command in ("Xvfb", "dbus-run-session", "xdotool", "xwininfo", "import", "gdbus", "convert"):
        if not shutil.which(command):
            parser.error(f"required command not found: {command}")
    if not args.binary.is_file():
        parser.error(f"build executable first: {args.binary}")
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-notification-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-notification-") as temporary:
        root = Path(temporary)
        for directory in ("home", "config/midnight-wm", "config-system", "data/applications", "data-system", "runtime"):
            (root / directory).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        (root / "config/midnight-wm/config.ini").write_text("[Workspace]\nui_scale=1.0\n")
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
                                   XDG_DATA_HOME=str(root / "data"), XDG_DATA_DIRS=str(root / "data-system"),
                                   XDG_RUNTIME_DIR=str(root / "runtime"), LC_ALL="C")
                process = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                                            "--session", "--binary", str(args.binary), "--output", str(args.output)],
                                           env=environment, start_new_session=True)
                try:
                    if process.wait(timeout=60):
                        raise RuntimeError(f"Notification smoke test failed; see {args.output}")
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
