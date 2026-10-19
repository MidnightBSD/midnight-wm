#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Isolated Xorg dummy-driver checks using real RandR topology notifications.

Requires dummy 0.4.x (HW_SKIP_CONSOLE), Xorg, xrandr, xauth and the normal X11
smoke tools. Never connects to an inherited DISPLAY or changes physical outputs.
"""
import argparse
import configparser
import os
from pathlib import Path
import re
import secrets
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, window_for_size, window_position, geometry, visible_window

REPO = Path(__file__).resolve().parents[1]


def session(args):
    root = args.session
    binary = args.binary
    fixture = args.fixture
    config = root / 'config/midnight-wm/config.ini'
    config.write_text('[Workspace]\ncompositing=false\nui_scale=1.0\n[Session]\n'
        'restore_folders=true\nsave_on_logout=true\nrestore_applications=true\nsave_application_checkpoints=true\n')
    autostart = root / 'config/autostart/fixture.desktop'
    autostart.parent.mkdir()
    autostart.write_text('[Desktop Entry]\nType=Application\nName=RandR fixture\n'
        f'Exec={fixture} --windows yes --output {root / "clients"} --literal randr-session\n')
    folder = root / 'Folder'
    folder.mkdir()
    (folder / 'keep.txt').write_text('survives topology and session changes\n')
    known = set()
    owned = set()
    process = None
    log = None

    def layout(name, *arguments):
        run('xrandr', '--fb', '3840x1440', *arguments)
        monitors = run('xrandr', '--listmonitors')
        (root / (name + '-monitors.txt')).write_text(monitors)
        assert geometry(root_window) == (3840, 1440), 'framebuffer changed during topology test'
        return monitors

    def capture(name):
        (root / (name + '-windows.txt')).write_text(run('xwininfo', '-root', '-tree'))
        if shutil.which('import'):
            run('import', '-window', 'root', str(root / (name + '.png')))

    def start():
        nonlocal process, log
        log = (root / f'wm-{time.time_ns()}.log').open('w')
        process = subprocess.Popen([str(binary)], stdout=log, stderr=subprocess.STDOUT)
        paths = until(lambda: set((root / 'clients').glob('*.ini')) - known, 'XSMP fixture not ready')
        assert len(paths) == 1, 'duplicate autostart/restore'
        path = paths.pop()
        known.add(path)
        owned.add(int(path.stem))
        keys = configparser.ConfigParser(interpolation=None)
        keys.read(path)
        windows = {name: hex(int(value)) for name, value in keys['Windows'].items()}
        for window in windows.values():
            until(lambda w=window: '=' in run('xprop', '-id', w, '_NET_FRAME_EXTENTS'), 'client not managed')
        return keys['Client'], windows

    def stop():
        if process and process.poll() is None:
            process.terminate()
            assert process.wait(timeout=12) == 0, 'companion failed on termination'
        if log:
            log.close()
        for pid in owned:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        owned.clear()

    def bbox(window):
        x, y = window_position(window)
        w, h = geometry(window)
        value = run('xprop', '-id', window, '_NET_FRAME_EXTENTS')
        match = re.search(r'=\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)', value)
        if match:
            left, right, top, bottom = map(int, match.groups())
            return x-left, y-top, w+left+right, h+top+bottom
        return x, y, w, h

    def panel(x, y, width):
        def matches():
            window = window_for_size(width, 30)
            return window and window_position(window) == (x, y)
        until(matches, f'panel did not follow primary output to {(x,y,width)}')

    def state(window, flags):
        run('wmctrl', '-ir', window, '-b', 'add,' + flags)
        for flag in flags.split(','):
            until(lambda f=flag: '_NET_WM_STATE_' + f.upper() in run('xprop', '-id', window, '_NET_WM_STATE'),
                  f'{flag} did not apply')

    def reachable(window, areas):
        x, y, w, _ = bbox(window)
        return any(min(x+w, ax+aw)-max(x, ax) >= min(80, w) and ay <= y <= ay+ah-28
                   for ax, ay, aw, ah in areas)

    root_window = re.search(r'Window id:\s*(0x[0-9a-f]+)', run('xwininfo', '-root')).group(1)
    try:
        run('xrandr', '--addmode', 'DUMMY1', '1280x720')
        topology = layout('initial', '--output', 'DUMMY0', '--mode', '1920x1080', '--pos', '0x0', '--primary',
            '--output', 'DUMMY1', '--mode', '1280x720', '--pos', '1920x0')
        assert 'Monitors: 2' in topology and 'DUMMY1' in topology, 'dummy driver must support multiple RandR outputs'
        first, windows = start()
        panel(0, 0, 1920)
        second = subprocess.run([str(binary.with_name('midnight-wm'))], capture_output=True, timeout=5)
        assert second.returncode != 0, 'second WM took over test screen'
        for name in ('normal', 'shade', 'max', 'full'):
            run('wmctrl', '-ir', windows[name], '-e', '0,2100,100,340,220')
            until(lambda n=name: bbox(windows[n])[0] == 2100, 'window did not move onto secondary output')
        state(windows['shade'], 'shaded')
        state(windows['max'], 'maximized_horz,maximized_vert')
        state(windows['full'], 'fullscreen')
        until(lambda: bbox(windows['full']) == (1920,0,1280,720), 'initial fullscreen bounds wrong')
        capture('initial')
        layout('primary-right', '--output', 'DUMMY1', '--primary')
        panel(1920, 0, 1280)
        until(lambda: bbox(windows['max']) == (1920,30,1280,690), 'maximize did not follow new panel work area')
        assert bbox(windows['full']) == (1920,0,1280,720)
        capture('primary-right')
        layout('offset-gap', '--output', 'DUMMY1', '--pos', '2400x300')
        panel(2400, 300, 1280)
        until(lambda: bbox(windows['full']) == (2400,300,1280,720), 'fullscreen lost named monitor after offset change')
        until(lambda: bbox(windows['max']) == (2400,330,1280,690), 'maximize lost named monitor after offset change')
        areas = [(0,0,1920,1080), (2400,330,1280,690)]
        for name in ('normal', 'shade'):
            assert reachable(windows[name], areas), f'{name} title stranded in output gap'
        run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.FileManager1', '--object-path',
            '/org/freedesktop/FileManager1', '--method', 'org.freedesktop.FileManager1.ShowFolders', repr([folder.as_uri()]), '')
        folder_window = until(lambda: visible_window('Folder: ' + str(folder)), 'folder not opened')
        until(lambda: reachable(folder_window, areas), 'folder opened outside visible outputs')
        capture('offset-gap')
        layout('secondary-disabled', '--output', 'DUMMY0', '--primary', '--output', 'DUMMY1', '--off')
        panel(0, 0, 1920)
        until(lambda: bbox(windows['full']) == (0,0,1920,1080), 'fullscreen did not recover after output disable')
        until(lambda: bbox(windows['max']) == (0,30,1920,1050), 'maximize did not recover after output disable')
        for window in (windows['normal'], windows['shade'], folder_window):
            until(lambda w=window: reachable(w, [(0,30,1920,1050)]), 'window not reachable after output disable')
        assert '_NET_WM_STATE_SHADED' in run('xprop','-id',windows['shade'],'_NET_WM_STATE')
        capture('secondary-disabled')
        layout('secondary-enabled', '--output', 'DUMMY1', '--mode', '1280x720', '--pos', '2400x300', '--primary')
        panel(2400, 300, 1280)
        # Save a window on an output that is absent at the next login.
        run('wmctrl', '-ir', windows['normal'], '-e', '0,2550,400,340,220')
        until(lambda: bbox(windows['normal'])[0] == 2550, 'save window did not move to secondary output')
        x, y = window_position(folder_window)
        run('xdotool', 'mousemove', '--sync', str(x+90), str(y+15), 'mousedown', '1',
            'mousemove', '--sync', '2640', '415', 'mouseup', '1')
        until(lambda: window_position(folder_window) == (2550,400), 'folder did not move onto output before save')
        snapshot = root / 'state/midnight-wm/applications.ini'
        os.kill(process.pid, signal.SIGUSR1)
        until(snapshot.is_file, 'application checkpoint not saved')
        saved = snapshot.read_bytes()
        stop()
        layout('login-one-output', '--output', 'DUMMY0', '--primary', '--output', 'DUMMY1', '--off')
        restored, restored_windows = start()
        panel(0, 0, 1920)
        assert restored['id'] == restored['previous'] == first['id'], 'session client identity not restored'
        until(lambda: bbox(restored_windows['normal'])[0] == 1570, 'saved off-output window did not recover at login')
        until(lambda: bbox(restored_windows['max']) == (0,30,1920,1050), 'saved maximize did not recover at login')
        until(lambda: bbox(restored_windows['full']) == (0,0,1920,1080), 'saved fullscreen did not recover at login')
        assert '_NET_WM_STATE_SHADED' in run('xprop','-id',restored_windows['shade'],'_NET_WM_STATE')
        folder_window = until(lambda: visible_window('Folder: ' + str(folder)), 'built-in folder did not restore')
        assert reachable(folder_window, [(0,30,1920,1050)])
        assert snapshot.read_bytes() == saved, 'login rewrote saved checkpoint'
        capture('restored-one-output')
        print('PASS: genuine RandR primary/offset/gap/disable/re-enable events with fixed framebuffer, named-monitor states, reachable windows/folders and one-output session restore')
    except BaseException:
        capture('failure')
        raise
    finally:
        stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module-path', type=Path, help='explicit private module root containing drivers/dummy_drv.so (dummy 0.4.x)')
    parser.add_argument('--binary', type=Path, default=REPO / 'build/midnightbsd-session')
    parser.add_argument('--fixture', type=Path, default=REPO / 'build/xsmp_restore_fixture')
    parser.add_argument('--session', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.binary, args.fixture = args.binary.resolve(), args.fixture.resolve()
    if args.session:
        session(args)
        return
    if not args.module_path or not (args.module_path / 'drivers/dummy_drv.so').is_file():
        parser.error('provide --module-path with a private dummy 0.4.x module; see docs/session-validation.md')
    for command in ('Xorg', 'xrandr', 'xauth', 'dbus-run-session', 'gdbus', 'xdotool', 'wmctrl', 'xprop', 'xwininfo'):
        if not shutil.which(command):
            parser.error('required command missing: ' + command)
    root = Path(tempfile.mkdtemp(prefix='midnight-session-randr-'))
    print(f'Artifacts: {root}', flush=True)
    for name in ('home', 'config/midnight-wm', 'config-system', 'data', 'data-system', 'state', 'runtime', 'clients'):
        (root / name).mkdir(parents=True, exist_ok=True)
    (root / 'runtime').chmod(0o700)
    auth = root / 'Xauthority'
    cookie = secrets.token_hex(16)
    subprocess.run(['xauth', '-f', str(auth)], input=f'add :99 . {cookie}\n', text=True, check=True, capture_output=True)
    env = os.environ.copy()
    for name in ('DISPLAY', 'XAUTHORITY', 'DBUS_SESSION_BUS_ADDRESS', 'DBUS_SESSION_BUS_PID', 'SESSION_MANAGER',
                 'ICEAUTHORITY', 'SSH_AUTH_SOCK', 'SSH_AGENT_PID', 'MIDNIGHT_WM_RESTORE_PLAN', 'MIDNIGHT_WM_RESTORE_MANAGER',
                 'MIDNIGHT_WM_WINDOW_FD', 'MIDNIGHT_WM_WINDOW_REPORT', 'MIDNIGHT_WM_TEST_SYNTHETIC_RESIZE'):
        env.pop(name, None)
    env.update(HOME=str(root / 'home'), XDG_CONFIG_HOME=str(root / 'config'), XDG_CONFIG_DIRS=str(root / 'config-system'),
        XDG_DATA_HOME=str(root / 'data'), XDG_DATA_DIRS=str(root / 'data-system'), XDG_STATE_HOME=str(root / 'state'),
        XDG_RUNTIME_DIR=str(root / 'runtime'), XDG_CURRENT_DESKTOP='MidnightWorkspace', XAUTHORITY=str(auth), NO_AT_BRIDGE='1')
    read_fd, write_fd = os.pipe()
    with (root / 'server.log').open('w') as log:
        server = subprocess.Popen(['Xorg', '-displayfd', str(write_fd), '-config', str(REPO / 'tests/data/xorg-dummy.conf'),
            '-modulepath', str(args.module_path.resolve()) + ',/usr/local/lib/xorg/modules', '-logfile', str(root / 'Xorg.log'),
            '-auth', str(auth), '-nolisten', 'tcp', '-noreset', '-novtswitch'], env=env, pass_fds=(write_fd,), stdout=log, stderr=subprocess.STDOUT)
    os.close(write_fd)
    try:
        assert select.select([read_fd], [], [], 15)[0], f'Xorg startup timed out; see {root}'
        with os.fdopen(read_fd) as stream:
            display = stream.readline().strip()
        assert display.isdecimal(), f'Xorg did not start; see {root}'
        env['DISPLAY'] = ':' + display
        subprocess.run(['xauth', '-f', str(auth)], input=f'add {env["DISPLAY"]} . {cookie}\n', text=True, check=True, capture_output=True)
        child = subprocess.Popen(['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve()), '--session', str(root),
            '--binary', str(args.binary), '--fixture', str(args.fixture)], env=env, start_new_session=True)
        try:
            assert child.wait(timeout=110) == 0, f'RandR check failed; see {root}'
        finally:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            child.wait(timeout=5)
    finally:
        server.terminate()
        server.wait(timeout=10)
        auth.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
