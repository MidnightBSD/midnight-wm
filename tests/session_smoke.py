#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise opt-in folder sessions across independent WM lifetimes in Xvfb."""
import argparse
import configparser
import contextlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from settings_smoke import run, until, geometry, window_position, window_for_size


def windows(path, visible=False):
    arguments = ["xdotool", "search"]
    if visible:
        arguments.append("--onlyvisible")
    arguments += ["--name", "^Folder: " + str(path) + "$"]
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=5)
    return result.stdout.splitlines() if result.returncode == 0 else []


def session(args):
    root = Path(os.environ["HOME"]).parent
    config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
    snapshot = Path(os.environ["XDG_STATE_HOME"]) / "midnight-wm/session.ini"
    paths = [root / name for name in ("normal folder", "shaded", "maximized")]
    for path in paths:
        path.mkdir()

    def configure(restore=True, save=True):
        config.write_text("[Workspace]\nui_scale=1.0\ncompositing=false\n"
                          f"[Session]\nrestore_folders={str(restore).lower()}\nsave_on_logout={str(save).lower()}\n")

    def start():
        log = (args.output / f"{args.width}x{args.height}-wm-{time.time_ns()}.log").open("w")
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        log.close()
        until(lambda: window_for_size(args.width, 30), "panel did not appear")
        until(lambda: "org.freedesktop.FileManager1" in run("gdbus", "call", "--session",
              "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus",
              "--method", "org.freedesktop.DBus.ListNames"), "FileManager1 missing")
        return wm

    def workspace(index):
        run("xdotool", "set_desktop", str(index))
        until(lambda: run("xdotool", "get_desktop").strip() == str(index), "workspace not selected")

    def open_folder(path):
        run("gdbus", "call", "--session", "--dest", "org.freedesktop.FileManager1",
            "--object-path", "/org/freedesktop/FileManager1", "--method",
            "org.freedesktop.FileManager1.ShowFolders", "['" + path.as_uri() + "']", "")
        return until(lambda: (windows(path, True) or [None])[0], "folder missing")

    def title_click(window, button=1, twice=False):
        x, y = window_position(window)
        run("xdotool", "mousemove", str(x + 90), str(y + 15))
        if twice:
            run("xdotool", "click", "--repeat", "2", "--delay", "90", str(button))
        else:
            run("xdotool", "click", str(button))

    def exit_dialog():
        run("xdotool", "mousemove", "12", "12", "click", "1")
        time.sleep(.25)
        run("xdotool", "mousemove", "40", "334", "click", "1")
        return until(lambda: window_for_size(380, 174), "exit dialog missing")

    def logout(wm):
        exit_dialog()
        run("xdotool", "key", "Return")
        assert wm.wait(timeout=10) == 0, "WM failed on logout"

    configure()
    autostart = None
    if args.binary.name == "midnightbsd-session":
        # A real autostart application must inherit the already authenticated
        # manager, bus, and reachable agent before it tries to register.
        probe = root / "autostart-probe.py"
        probe.write_text("""import json, os, socket, subprocess, sys
from pathlib import Path
authority = Path(os.environ['ICEAUTHORITY'])
assert authority.stat().st_mode & 0o777 == 0o600
assert os.environ['SESSION_MANAGER']
subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus',
 '--object-path', '/org/freedesktop/DBus', '--method', 'org.freedesktop.DBus.ListNames'],
 check=True, stdout=subprocess.DEVNULL)
agent = os.environ.get('SSH_AUTH_SOCK')
if agent:
 with socket.socket(socket.AF_UNIX) as peer: peer.connect(agent)
subprocess.run([sys.argv[1], 'normal'], check=True, timeout=15)
Path(sys.argv[2]).write_text(json.dumps({'manager': os.environ['SESSION_MANAGER'],
 'authority': str(authority), 'agent': bool(agent)}))
""")
        directory = Path(os.environ["XDG_CONFIG_HOME"]) / "autostart"
        directory.mkdir()
        autostart = directory / "midnight-xsmp-probe.desktop"
        autostart.write_text('[Desktop Entry]\nType=Application\nName=XSMP probe\nExec="' + sys.executable +
            '" "' + str(probe) + '" "' + str(args.binary.parent / "xsmp_client_fixture") +
            '" "' + str(root / "autostart-ready.json") + '"\nOnlyShowIn=MidnightWorkspace;\n')
    wm = start()
    try:
        if autostart:
            ready = root / "autostart-ready.json"
            until(ready.exists, "autostart did not inherit a working XSMP manager/bus/agent", timeout=20)
            info = json.loads(ready.read_text())
            assert Path(info["authority"]).is_file()
            autostart.unlink()
        second = subprocess.run([str(args.binary)], capture_output=True, timeout=5)
        assert second.returncode != 0, "second WM took over"
        normal = open_folder(paths[0])
        normal_geometry = (window_position(normal), geometry(normal))
        workspace(1)
        shaded = open_folder(paths[1])
        title_click(shaded, 2)
        until(lambda: geometry(shaded)[1] == 34, "middle title click did not shade folder")
        workspace(2)
        maximized = open_folder(paths[2])
        title_click(maximized, twice=True)
        until(lambda: geometry(maximized) == (args.width, args.height - 30), "folder did not maximize")
        exit_dialog()
        run("xdotool", "key", "Escape")
        until(lambda: not window_for_size(380, 174), "cancel did not close logout dialog")
        assert wm.poll() is None and not snapshot.exists(), "cancelled logout saved or stopped the session"
        logout(wm)
        assert snapshot.exists() and snapshot.stat().st_mode & 0o777 == 0o600
        saved = configparser.ConfigParser()
        saved.read(snapshot)
        assert saved["Session"]["workspace"] == "2" and saved["Session"]["folders"] == "3"
        entries = {saved[group]["path"]: saved[group] for group in saved if group.startswith("Folder ")}
        assert entries[str(paths[1])]["shaded"] == "true" and entries[str(paths[1])]["workspace"] == "1"
        assert entries[str(paths[2])]["maximized"] == "true" and entries[str(paths[2])]["workspace"] == "2"
        original = snapshot.read_bytes()

        wm = start()
        until(lambda: run("xdotool", "get_desktop").strip() == "2", "selected workspace not restored")
        maximized = until(lambda: (windows(paths[2], True) or [None])[0], "maximized folder not restored")
        assert geometry(maximized) == (args.width, args.height - 30)
        assert not windows(paths[0], True) and not windows(paths[1], True), "other workspace folders were mapped"
        workspace(0)
        normal = until(lambda: (windows(paths[0], True) or [None])[0], "normal folder not restored")
        assert (window_position(normal), geometry(normal)) == normal_geometry
        workspace(1)
        shaded = until(lambda: (windows(paths[1], True) or [None])[0], "shaded folder not restored")
        assert geometry(shaded)[1] == 34
        # Task activation must expand a restored shaded folder.
        run("xdotool", "mousemove", "240", "16", "click", "1")
        until(lambda: geometry(shaded)[1] == 430, "task activation did not expand folder")
        workspace(2)
        title_click(maximized, twice=True)
        until(lambda: geometry(maximized) == (620, 430), "normal geometry not retained across restore/maximize")
        run("import", "-window", "root", str(args.output / f"{args.width}x{args.height}-restored.png"))
        wm.terminate()
        wm.wait(timeout=10)

        # Seed an absent monitor and a vanished folder; keep other valid entries.
        snapshot.write_bytes(original)
        saved.read(snapshot)
        saved["Folder 0"]["x"] = "30000"
        saved["Folder 0"]["y"] = "30000"
        saved["Folder 0"]["width"] = "3000"
        saved["Folder 0"]["height"] = "2000"
        saved["Folder 2"]["path"] = str(root / "missing-folder")
        with snapshot.open("w") as stream:
            saved.write(stream)
        wm = start()
        until(lambda: windows(paths[0]), "recoverable folder missing")
        workspace(0)
        normal = windows(paths[0])[0]
        x, y = window_position(normal)
        width, height = geometry(normal)
        assert x >= 0 and y >= 30 and x + width <= args.width and y + height <= args.height
        assert not windows(root / "missing-folder"), "vanished folder was reopened"
        wm.terminate()
        wm.wait(timeout=10)

        # Defaults/fresh startup do not consume or overwrite saved state.
        configure(False, False)
        preserved = snapshot.read_bytes()
        wm = start()
        time.sleep(.3)
        assert not any(windows(path) for path in paths), "fresh session restored folders"
        wm.terminate()
        wm.wait(timeout=10)
        assert snapshot.read_bytes() == preserved

        configure()
        snapshot.write_text("[Session]\nversion=100\n")
        corrupt = snapshot.read_bytes()
        wm = start()
        assert not any(windows(path) for path in paths), "invalid snapshot restored folders"
        logout(wm)
        assert snapshot.read_bytes() == corrupt, "invalid snapshot overwritten on logout"

        # Explicitly clearing rejected state permits a new save in the same run.
        wm = start()
        snapshot.unlink()
        open_folder(paths[0])
        logout(wm)
        repaired = configparser.ConfigParser()
        repaired.read(snapshot)
        assert repaired["Session"]["folders"] == "1", "clearing rejected state did not allow saving again"
        snapshot.write_bytes(corrupt)

        # The Settings Session page applies preferences and clears rejected state.
        configure(False, False)
        config.write_text(config.read_text() + "\n# Retain this comment\n[Unrelated]\nvalue=keep\n")
        wm = start()
        settings_env = os.environ.copy()
        settings_env["XDG_DATA_DIRS"] = "/usr/local/share:/usr/share"
        settings = subprocess.Popen([str(args.binary.parent / "midnightbsd-settings")], env=settings_env)
        try:
            from settings_smoke import visible_window
            window = until(lambda: visible_window("Midnight Workspace Settings"), "settings missing")
            run("xdotool", "windowactivate", "--sync", window)
            run("xdotool", "key", "--clearmodifiers", "ctrl+Page_Down")
            time.sleep(.2)
            run("xdotool", "key", "--clearmodifiers", "ctrl+Page_Down")
            time.sleep(.2)
            run("xdotool", "key", "--clearmodifiers", "ctrl+Page_Down")
            time.sleep(.2)
            run("import", "-window", "root", str(args.output / f"{args.width}x{args.height}-settings-before-clear.png"))
            run("xdotool", "key", "--clearmodifiers", "alt+r", "alt+s", "alt+a")
            until(lambda: "restore_folders=true" in config.read_text() and "save_on_logout=true" in config.read_text(),
                  "Session checkboxes did not save preferences")
            assert "# Retain this comment" in config.read_text() and "value=keep" in config.read_text()
            run("xdotool", "key", "--clearmodifiers", "alt+l", "alt+k", "alt+a")
            until(lambda: "restore_applications=true" in config.read_text() and
                  "save_application_checkpoints=true" in config.read_text(),
                  "application session controls did not save preferences")
            run("xdotool", "key", "--clearmodifiers", "alt+w", "alt+a")
            until(lambda: "restore_desktop_applications=true" in config.read_text(),
                  "best-effort relaunch opt-in did not save")
            run("xdotool", "key", "--clearmodifiers", "alt+w", "alt+a")
            until(lambda: "restore_desktop_applications=false" in config.read_text(),
                  "best-effort relaunch could not be disabled")
            applications = snapshot.parent / "applications.ini"
            applications.write_text("[Applications]\nversion=999\n")
            applications.chmod(0o600)
            previous_folder = snapshot.read_bytes()
            run("xdotool", "key", "--clearmodifiers", "alt+e")
            until(lambda: not applications.exists(), "clear application session failed")
            assert snapshot.read_bytes() == previous_folder, "application clear changed folder snapshot"
            assert wm.poll() is None, "clearing application state ended desktop"
            # A failed clear must leave the target and other session intact.
            applications.mkdir()
            run("xdotool", "key", "--clearmodifiers", "alt+e")
            time.sleep(.2)
            assert applications.is_dir() and snapshot.read_bytes() == previous_folder
            applications.rmdir()
            # Starting fresh changes restoration only; saving remains enabled.
            run("xdotool", "key", "--clearmodifiers", "alt+r", "alt+l", "alt+a")
            until(lambda: "restore_applications=false" in config.read_text() and
                  "restore_folders=false" in config.read_text(), "start-fresh preferences did not save")
            assert "save_application_checkpoints=true" in config.read_text()
            assert "save_on_logout=true" in config.read_text()
            run("xdotool", "key", "--clearmodifiers", "alt+f")
            until(lambda: not snapshot.exists(), "clear saved folder session failed")
            run("import", "-window", "root", str(args.output / f"{args.width}x{args.height}-settings.png"))
        finally:
            settings.terminate()
            settings.wait(timeout=5)
        time.sleep(3)  # Allow stable live-config polling to enable save-on-logout.
        logout(wm)
        saved.read(snapshot)
        assert saved["Session"]["folders"] == "0", "live save preference was not applied"
        print(f"{args.width}x{args.height}: folder save/restore, states, workspaces, cancellation, geometry recovery, fresh startup, corrupt preservation, application session preferences and independent clears passed.", flush=True)
    finally:
        if wm.poll() is None:
            wm.terminate()
            wm.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/midnight-wm"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--matrix", action="store_true")
    parser.add_argument("--session", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    if args.session:
        session(args)
        return
    args.output = (args.output or Path(tempfile.mkdtemp(prefix="midnight-session-results-"))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"Results: {args.output}", flush=True)
    sizes = [(1280, 720), (1920, 1080), (2560, 1440), (3440, 1440), (3840, 2160)] if args.matrix else [(args.width, args.height)]
    for width, height in sizes:
        with tempfile.TemporaryDirectory(prefix="midnight-session-") as temporary:
            root = Path(temporary)
            for name in ("home", "config/midnight-wm", "config-system", "data", "data-system", "state", "runtime"):
                (root / name).mkdir(parents=True)
            (root / "runtime").chmod(0o700)
            read_fd, write_fd = os.pipe()
            xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", f"{width}x{height}x24",
                                     "-noreset", "-nolisten", "tcp"], pass_fds=(write_fd,),
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.close(write_fd)
            try:
                with os.fdopen(read_fd) as stream:
                    display = stream.readline().strip()
                if not display:
                    raise RuntimeError("Xvfb did not start")
                env = os.environ.copy()
                for name in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_PID", "SSH_AUTH_SOCK", "SSH_AGENT_PID",
                             "SESSION_MANAGER", "ICEAUTHORITY", "LD_PRELOAD"):
                    env.pop(name, None)
                env.update(DISPLAY=":" + display, HOME=str(root / "home"), XDG_CONFIG_HOME=str(root / "config"),
                           XDG_CONFIG_DIRS=str(root / "config-system"), XDG_DATA_HOME=str(root / "data"),
                           XDG_DATA_DIRS=str(root / "data-system"), XDG_STATE_HOME=str(root / "state"),
                           XDG_RUNTIME_DIR=str(root / "runtime"), XDG_CURRENT_DESKTOP="MidnightWorkspace", NO_AT_BRIDGE="1")
                process = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                                            "--session", "--binary", str(args.binary), "--output", str(args.output),
                                            "--width", str(width), "--height", str(height)], env=env, start_new_session=True)
                try:
                    if process.wait(timeout=120):
                        raise RuntimeError("Session smoke failed; see " + str(args.output))
                finally:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
