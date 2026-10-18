#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Isolated MPRIS and speaker-popup checks; restores host OSS volume after testing."""
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

SERVICE = "org.mpris.MediaPlayer2.MidnightTest"
PATH = "/org/mpris/MediaPlayer2"
PLAYER = "org.mpris.MediaPlayer2.Player"
CONTROL = "org.midnight.Test"

def provider(args):
    import gi
    from gi.repository import Gio, GLib
    service = "org.mpris.MediaPlayer2." + args.player_name
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    properties = {"PlaybackStatus": GLib.Variant("s", "Paused"),
                  "Metadata": GLib.Variant("a{sv}", {"xesam:title": GLib.Variant("s", "Midnight test track"),
                                                       "xesam:artist": GLib.Variant("as", ["Test artist"])})}
    for key in ("CanControl", "CanPlay", "CanPause", "CanGoNext", "CanGoPrevious"):
        properties[key] = GLib.Variant("b", True)
    xml = '<node><interface name="' + PLAYER + '">'
    for name in ("Play", "Pause", "Next", "Previous"):
        xml += '<method name="' + name + '"/>'
    for key, value in properties.items():
        xml += '<property name="' + key + '" type="' + value.get_type_string() + '" access="read"/>'
    xml += '</interface><interface name="' + CONTROL + '"><method name="Set"><arg type="s" direction="in"/>'
    xml += '<arg type="b" direction="in"/></method></interface></node>'
    node = Gio.DBusNodeInfo.new_for_xml(xml)
    def changed(values):
        properties.update(values)
        bus.emit_signal(None, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                        GLib.Variant("(sa{sv}as)", (PLAYER, values, [])))
    def method(connection, sender, path, interface, name, parameters, invocation):
        if interface == CONTROL:
            key, value = parameters.unpack()
            changed({key: GLib.Variant("b", value)})
        else:
            with (args.output / (args.player_name + "-commands.log")).open("a") as log:
                log.write(name + "\n")
            if name in ("Play", "Pause"):
                changed({"PlaybackStatus": GLib.Variant("s", "Playing" if name == "Play" else "Paused")})
        invocation.return_value(None)
    for interface in node.interfaces:
        bus.register_object(PATH, interface, method,
                            lambda connection, sender, path, interface, prop: properties[prop], None)
    bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                  GLib.Variant("(su)", (service, 4)), GLib.VariantType.new("(u)"), Gio.DBusCallFlags.NONE, 2000, None)
    (args.output / (args.player_name + ".ready")).touch()
    GLib.MainLoop().run()

def session(args):
    for name in ("MidnightTest", "ZSecondTest"):
        for suffix in (".ready", "-commands.log"):
            (args.output / (name + suffix)).unlink(missing_ok=True)
    saved = None
    provider_process = None
    second_process = None
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        try:
            until(lambda: "_NET_SUPPORTING_WM_CHECK(WINDOW)" in run("xprop", "-root", "_NET_SUPPORTING_WM_CHECK"),
                  "window manager did not start")
            other = subprocess.run([str(args.binary)], capture_output=True, text=True, timeout=5)
            assert other.returncode != 0, "second WM took over"
            def speaker():
                run("xdotool", "mousemove", "1173", "16", "click", "1")
            speaker()
            popup = until(lambda: window_for_size(260, 230), "speaker popup did not open")
            x, y = window_position(popup)
            assert 0 <= x <= 1020 and 30 <= y <= 490
            run("import", "-window", popup, str(args.output / "no-player.png"))
            speaker()
            until(lambda: not window_for_size(260, 230), "speaker click did not toggle popup off")
            provider_process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--provider",
                                                  "--output", str(args.output)], stdout=log, stderr=subprocess.STDOUT)
            until(lambda: (args.output / "MidnightTest.ready").exists(), "fake media player did not start")
            speaker()
            popup = until(lambda: window_for_size(260, 230), "speaker popup did not reopen")
            x, y = window_position(popup)
            time.sleep(.6)
            run("import", "-window", popup, str(args.output / "with-player.png"))
            def click(relative_x, relative_y):
                run("xdotool", "mousemove", str(x + relative_x), str(y + relative_y), "click", "1")
            def commands():
                path = args.output / "MidnightTest-commands.log"
                return path.read_text().splitlines() if path.exists() else []
            for index, command in ((1, "Play"), (1, "Pause"), (0, "Previous"), (2, "Next")):
                click(50 + index * 76, 198)
                until(lambda: command in commands(), "MPRIS command not delivered: " + command)
                time.sleep(.2)
            run("gdbus", "call", "--session", "--dest", SERVICE, "--object-path", PATH,
                "--method", CONTROL + ".Set", "CanGoNext", "false")
            time.sleep(.3)
            before = commands()
            click(202, 198)
            time.sleep(.3)
            assert commands() == before, "disabled Next action was invoked"
            run("gdbus", "call", "--session", "--dest", SERVICE, "--object-path", PATH,
                "--method", CONTROL + ".Set", "CanControl", "false")
            time.sleep(.3)
            click(126, 198)
            time.sleep(.3)
            assert commands() == before, "CanControl=false was ignored"
            # A newly discovered player can be selected independently; exit falls
            # back to the remaining player without retaining a stale bus proxy.
            second_process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--provider",
                                                "--player-name", "ZSecondTest", "--output", str(args.output)],
                                               stdout=log, stderr=subprocess.STDOUT)
            until(lambda: (args.output / "ZSecondTest.ready").exists(), "second player did not start")
            time.sleep(.5)
            click(100, 117)
            click(126, 198)
            second_log = args.output / "ZSecondTest-commands.log"
            until(lambda: second_log.exists() and "Play" in second_log.read_text(),
                  "player selector did not route Play to the second player")
            run("import", "-window", popup, str(args.output / "second-player.png"))
            second_process.terminate()
            second_process.wait(timeout=5)
            second_process = None
            run("gdbus", "call", "--session", "--dest", SERVICE, "--object-path", PATH,
                "--method", CONTROL + ".Set", "CanControl", "true")
            time.sleep(.5)
            play_count = commands().count("Play")
            click(126, 198)
            until(lambda: commands().count("Play") > play_count, "player exit did not restore first player")
            if shutil.which("mixer"):
                levels = run("mixer", "-s")
                match = re.search(r"\b(vol|pcm) (\d+):(\d+)", levels)
                if match:
                    saved = match.groups()
                    (args.output / "original-mixer.txt").write_text(levels)
                    click(130, 44)
                    until(lambda: f"{saved[0]} 50:50" in run("mixer", "-s"), "slider did not set volume")
                    click(100, 79)
                    until(lambda: f"{saved[0]} 0:0" in run("mixer", "-s"), "mute did not silence mixer")
                    click(100, 79)
                    until(lambda: f"{saved[0]} 50:50" in run("mixer", "-s"), "unmute did not restore volume")
            provider_process.terminate()
            provider_process.wait(timeout=5)
            provider_process = None
            def empty_again():
                run("import", "-window", popup, str(args.output / "player-exited.png"))
                # Ignore mixer changes in the upper half: the media region must
                # return to precisely the initial no-player presentation.
                return run("convert", str(args.output / "player-exited.png"), "-crop", "260x128+0+102", "txt:-") == \
                    run("convert", str(args.output / "no-player.png"), "-crop", "260x128+0+102", "txt:-")
            until(empty_again, "departed player metadata remained visible")
            run("xdotool", "key", "Escape")
            until(lambda: not window_for_size(260, 230), "Escape did not dismiss popup")
            speaker()
            until(lambda: window_for_size(260, 230), "popup did not reopen after Escape")
            run("xdotool", "mousemove", "600", "600", "click", "1")
            until(lambda: not window_for_size(260, 230), "outside click did not dismiss popup")
            print("MPRIS discovery, commands, capability guards, multiple-player selection, exit/fallback, popup toggle/dismissal "
                  "and OSS slider/mute (when available) passed. Screenshots include track metadata.")
        finally:
            if saved:
                run("mixer", saved[0], saved[1] + ":" + saved[2])
                assert f"{saved[0]} {saved[1]}:{saved[2]}" in run("mixer", "-s"), "mixer restoration failed"
            if second_process:
                second_process.terminate()
                second_process.wait(timeout=5)
            if provider_process:
                provider_process.terminate()
                provider_process.wait(timeout=5)
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
    parser.add_argument("--provider", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--player-name", default="MidnightTest", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    if args.provider:
        provider(args)
        return
    if args.session:
        session(args)
        return
    for command in ("Xvfb", "dbus-run-session", "xdotool", "xwininfo", "import", "gdbus", "convert"):
        if not shutil.which(command):
            parser.error(f"required command not found: {command}")
    if not args.binary.is_file():
        parser.error(f"build executable first: {args.binary}")
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-media-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    with tempfile.TemporaryDirectory(prefix="midnight-media-") as temporary:
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
                        raise RuntimeError(f"Media smoke test failed; see {args.output}")
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
