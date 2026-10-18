#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Two isolated logins: XSMP checkpoint, relaunch, saved IDs and autostart."""
import argparse
import base64
import configparser
import os
from pathlib import Path
import signal
import re
import socket
import subprocess
import sys
import tempfile
import time


def until(check, message, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(.05)
    raise RuntimeError(message)


def read(path):
    keys = configparser.ConfigParser(interpolation=None)
    keys.read(path)
    return keys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/midnightbsd-session'))
    parser.add_argument('--fixture', type=Path, default=Path('build/xsmp_restore_fixture'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--placements', action='store_true')
    parser.add_argument('--width', type=int, default=1280)
    parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--restore-width', type=int)
    parser.add_argument('--restore-height', type=int)
    parser.add_argument('--changed-resolution-matrix', action='store_true')
    parser.add_argument('--matrix', action='store_true')
    args = parser.parse_args()
    if bool(args.restore_width) != bool(args.restore_height):
        parser.error('provide both --restore-width and --restore-height')
    if any(value is not None and not 640 <= value <= 8192 for value in
           (args.width, args.height, args.restore_width, args.restore_height)):
        parser.error('screen dimensions must be between 640 and 8192')
    if args.restore_width and not args.placements:
        parser.error('changed-resolution checks require --placements')
    args.binary, args.fixture = args.binary.resolve(), args.fixture.resolve()
    output = (args.output or Path(tempfile.mkdtemp(prefix='midnight-restore-results-'))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(f'Results: {output}', flush=True)
    if args.changed_resolution_matrix:
        for width, height in ((1280,720),(1920,1080),(2560,1440),(3440,1440),(3840,2160)):
            subprocess.run([sys.executable, str(Path(__file__).resolve()), '--placements',
                '--binary', str(args.binary), '--fixture', str(args.fixture), '--width', '3840', '--height', '2160',
                '--restore-width', str(width), '--restore-height', str(height),
                '--output', str(output / f'3840x2160-to-{width}x{height}')], check=True, timeout=100)
        return
    if args.matrix:
        for width, height in ((1280,720),(1920,1080),(2560,1440),(3440,1440),(3840,2160)):
            subprocess.run([sys.executable, str(Path(__file__).resolve()), '--placements',
                '--binary', str(args.binary), '--fixture', str(args.fixture), '--width', str(width), '--height', str(height),
                '--output', str(output / f'{width}x{height}')], check=True, timeout=100)
        return
    with tempfile.TemporaryDirectory(prefix='midnight-restore-') as temporary:
        root = Path(temporary)
        for name in ('home', 'config', 'config-system', 'data', 'data-system', 'state', 'runtime', 'clients'):
            (root / name).mkdir()
        (root / 'runtime').chmod(0o700)
        config = root / 'config/midnight-wm/config.ini'
        config.parent.mkdir()
        entry = root / 'config/autostart/restore-test.desktop'
        entry.parent.mkdir()
        literal = 'literal $(touch should-not-exist); with spaces'
        entry.write_text('[Desktop Entry]\nType=Application\nName=Restore fixture\n'
                         f'Exec={args.fixture} --output "{root / "clients"}" --literal "{literal}"' + (' --windows yes' if args.placements else '') + '\n')
        env = os.environ.copy()
        for name in ('DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'DBUS_SESSION_BUS_PID', 'SSH_AUTH_SOCK', 'SSH_AGENT_PID',
                     'SESSION_MANAGER', 'ICEAUTHORITY', 'MIDNIGHT_WM_RESTORE_PLAN', 'MIDNIGHT_WM_RESTORE_MANAGER'):
            env.pop(name, None)
        env.update(HOME=str(root / 'home'), XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_CONFIG_DIRS=str(root / 'config-system'), XDG_DATA_HOME=str(root / 'data'),
                   XDG_DATA_DIRS=str(root / 'data-system'), XDG_STATE_HOME=str(root / 'state'),
                   XDG_RUNTIME_DIR=str(root / 'runtime'), XDG_CURRENT_DESKTOP='MidnightWorkspace', NO_AT_BRIDGE='1')
        def server(width, height):
            read_fd, write_fd = os.pipe()
            with (output / f'xvfb-{width}x{height}.log').open('w') as log:
                display_server = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0',
                    f'{width}x{height}x24', '-nolisten', 'tcp'], pass_fds=(write_fd,), stdout=log, stderr=subprocess.STDOUT)
            os.close(write_fd)
            with os.fdopen(read_fd) as display:
                number = display.readline().strip()
            if not number:
                display_server.terminate(); display_server.wait(timeout=5)
                raise RuntimeError('Xvfb did not publish a display')
            env['DISPLAY'] = ':' + number
            return display_server
        xvfb = server(args.width, args.height)
        process = None
        known = set()
        owned_clients = set()
        snapshot = root / 'state/midnight-wm/applications.ini'
        def configure(restore, save=False):
            config.write_text('[Workspace]\ncompositing=false\n[Session]\n'
                              f'restore_applications={str(restore).lower()}\n'
                              f'save_application_checkpoints={str(save).lower()}\n')
        def start():
            nonlocal process
            (root / 'companion.pid').unlink(missing_ok=True)
            log = (output / f'session-{time.time_ns()}.log').open('w')
            # Record the companion PID from its explicitly owned child, then
            # replace that child with the real WM without changing the handoff.
            process = subprocess.Popen([str(args.binary), '--', sys.executable, '-c',
                "from pathlib import Path; import os,sys; Path(sys.argv[1]).write_text(str(os.getppid())); Path(sys.argv[1]).with_name('wm.pid').write_text(str(os.getpid())); os.execv(sys.argv[2],[sys.argv[2]])",
                str(root / 'companion.pid'), str(args.binary.with_name('midnight-wm'))],
                env=env, cwd=root / 'home', stdout=log, stderr=subprocess.STDOUT)
            log.close()
        def client():
            paths = until(lambda: set((root / 'clients').glob('*.ini')) - known, 'client did not register')
            assert len(paths) == 1, 'duplicate application launch'
            path = paths.pop()
            known.add(path)
            owned_clients.add(int(path.stem))
            info = read(path)['Client']
            info['record_path'] = str(path)
            return info
        def companion_pid(info):
            pid = int((root / 'companion.pid').read_text())
            assert pid > 1 and process.poll() is None
            return pid
        def command(*arguments):
            return subprocess.check_output(arguments, env=env, text=True, timeout=10)
        def handles(info):
            return {name: hex(int(value)) for name, value in read(Path(info['record_path']))['Windows'].items()}
        def geometry(window):
            info = command('xwininfo', '-id', window)
            values = [int(re.search(key + r':\s+(-?\d+)', info).group(1)) for key in
                ('Absolute upper-left X', 'Absolute upper-left Y', 'Width', 'Height')]
            extents = command('xprop', '-id', window, '_NET_FRAME_EXTENTS').split('=', 1)
            if len(extents) > 1:
                left, _, top, _ = map(int, extents[1].split(','))
                values[0] -= left; values[1] -= top
            return tuple(values)
        def state(window):
            return command('xprop', '-id', window, '_NET_WM_STATE')
        def stop(info, normal=False):
            nonlocal process
            pid = int((root / 'wm.pid').read_text()) if normal else companion_pid(info)
            os.kill(pid, signal.SIGTERM)
            assert process.wait(timeout=12) == 0, "WM did not exit cleanly after forwarded termination"
            process = None
        try:
            configure(False, True)
            start()
            first = client()
            assert first['previous'] == ''
            base = {}
            states = {'shade':'shaded', 'max':'maximized_horz,maximized_vert', 'full':'fullscreen',
                      'partial':'maximized_horz,shaded'}
            workspaces = {'shade':1,'max':2,'full':3,'partial':1}
            if args.placements:
                original_handles = handles(first)
                for index, (name, window) in enumerate(original_handles.items()):
                    until(lambda w=window: '=' in command('xprop','-id',w,'_NET_WM_DESKTOP'), 'window not managed')
                    width, height = (100,80) if name == 'tiny' else (350 + index*8, 230 + index*5)
                    command('wmctrl','-ir',window,'-e',f'0,{80+index*10},{90+index*8},{width},{height}')
                    time.sleep(.03)
                    base[name] = geometry(window)
                for name, value in states.items():
                    command('wmctrl','-ir',original_handles[name],'-b','add,'+value)
                for name, desktop in workspaces.items():
                    command('wmctrl','-ir',original_handles[name],'-t',str(desktop))
                until(lambda: '_NET_WM_STATE_SHADED' in state(original_handles['shade']), 'shade not applied')
            os.kill(companion_pid(first), signal.SIGUSR1)
            until(snapshot.is_file, 'checkpoint not saved')
            saved = snapshot.read_bytes()
            original = read(snapshot)
            original_id = base64.b64decode(original['Application 0']['client_id']).decode()
            assert original_id == first['id']
            if args.placements:
                records = [original[f'Window {i}'] for i in range(original.getint('Applications','windows'))]
                roles = {base64.b64decode(item['role']).decode() for item in records}
                assert roles == {'normal','shade','max','full','partial','tiny',''}, roles
                for item in records:
                    role = base64.b64decode(item['role']).decode() or 'roleless'
                    assert tuple(item.getint(key) for key in ('x','y','width','height')) == base[role], (role,dict(item),base[role])
            if args.placements:
                time.sleep(1.1)
                wm_pid = int((root / 'wm.pid').read_text())
                os.kill(wm_pid, signal.SIGSTOP)
                try:
                    os.kill(companion_pid(first), signal.SIGUSR1)
                    time.sleep(1.1)
                    assert snapshot.read_bytes() == saved, 'unresponsive WM replaced checkpoint'
                finally:
                    os.kill(wm_pid, signal.SIGCONT)
                # Normal WM shutdown must deliver a final fresh placement even
                # though the companion requests client saves after the WM exits.
                time.sleep(1.2)
                window = original_handles['normal']
                command('wmctrl','-ir',window,'-e',f'0,{args.width-520 if args.restore_width else 180},{args.height-440 if args.restore_height else 150},430,310')
                time.sleep(.1)
                base['normal'] = geometry(window)
                stop(first, normal=True)
                saved = snapshot.read_bytes()
                final = read(snapshot)
                item = next(final[section] for section in final.sections() if section.startswith('Window ') and base64.b64decode(final[section]['role']) == b'normal')
                assert tuple(item.getint(key) for key in ('x','y','width','height')) == base['normal'], 'final geometry was not captured'
            else:
                stop(first)
            if args.restore_width:
                source = (args.width, args.height)
                for pid in owned_clients:
                    try: os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError: pass
                owned_clients.clear()
                xvfb.terminate(); xvfb.wait(timeout=5)
                args.width, args.height = args.restore_width, args.restore_height
                xvfb = server(args.width, args.height)
                print(f'Restarting display for login: {source[0]}x{source[1]} -> {args.width}x{args.height}', flush=True)
            configure(True)
            start()
            restored = client()
            assert restored['id'] == restored['previous'] == original_id, 'saved ID not adopted'
            assert restored['literal'] == literal and restored['cwd'] == str(root / 'home')
            if args.placements:
                restored_handles = handles(restored)
                if args.restore_width:
                    until(lambda: '=' in command('xprop','-id',restored_handles['normal'],'_NET_FRAME_EXTENTS'), 'restored frame missing')
                    left, right, top, bottom = map(int, command('xprop','-id',restored_handles['normal'],
                        '_NET_FRAME_EXTENTS').split('=',1)[1].split(','))
                    area = list(map(int, command('xprop','-root','_NET_WORKAREA').split('=',1)[1].split(',')))[:4]
                    x, y, width, height = area
                    for name, (old_x, old_y, old_w, old_h) in base.items():
                        w, h = min(old_w, width-left-right), min(old_h, height-top-bottom)
                        base[name] = (max(x, min(old_x, x+width-left-right-w)),
                                      max(y, min(old_y, y+height-top-bottom-h)), w, h)
                    (output / 'changed-resolution.txt').write_text(
                        f'Current work area: {area}\nFrame extents: {(left,right,top,bottom)}\nExpected normal geometry: {base}\n')
                until(lambda: geometry(restored_handles['normal']) == base['normal'], 'normal placement not restored')
                for name, value in states.items():
                    for flag in value.split(','):
                        until(lambda n=name,f=flag: '_NET_WM_STATE_'+f.upper() in state(restored_handles[n]), f'{name} state missing')
                for name, desktop in workspaces.items():
                    until(lambda n=name,d=desktop: command('xprop','-id',restored_handles[n],'_NET_WM_DESKTOP').strip().endswith('= '+str(d)), 'workspace not restored')
                for name, value in states.items():
                    command('wmctrl','-ir',restored_handles[name],'-b','remove,'+value)
                    until(lambda n=name: geometry(restored_handles[n]) == base[n], f'{name} normal geometry lost')
                until(lambda: geometry(restored_handles['tiny']) == base['tiny'], 'tiny window enlarged')
                until(lambda: geometry(restored_handles['roleless']) == base['roleless'], 'roleless placement missing')
                assert command('xdotool','get_desktop').strip() == '0', 'restoration switched selected workspace'
            assert restored.getboolean('bus_reachable')
            assert restored['midnight_wm_restore_plan'] == restored['midnight_wm_restore_manager'] == ''
            assert restored['midnight_wm_window_fd'] == restored['midnight_wm_window_report'] == ''
            with socket.socket(socket.AF_UNIX) as peer:
                peer.connect(restored['ssh_auth_sock'])
            time.sleep(.5)
            assert set((root / 'clients').glob('*.ini')) == known, 'autostart duplicated restored application'
            assert snapshot.read_bytes() == saved, 'restoration rewrote checkpoint'
            duplicate_env = env.copy()
            for name in ('SESSION_MANAGER', 'ICEAUTHORITY', 'SSH_AUTH_SOCK', 'DBUS_SESSION_BUS_ADDRESS'):
                duplicate_env[name] = restored[name.lower()]
            duplicate = subprocess.Popen([str(args.fixture), '--output', str(root / 'clients'),
                '--literal', literal, '--sm-client-id', original_id], env=duplicate_env)
            try:
                retried = client()
                assert retried['id'] != original_id and retried['previous'] == original_id, 'saved ID adopted twice'
            finally:
                duplicate.terminate()
                duplicate.wait(timeout=5)
            stop(restored)
            # An unavailable executable must not stop other restores or overwrite
            # the source snapshot while its expected ID is still outstanding.
            keys = read(snapshot)
            keys['Applications']['count'] = '2'
            keys['Application 1'] = dict(keys['Application 0'])
            keys['Application 1']['client_id'] = base64.b64encode(b'missing-client').decode()
            keys['Application 1']['arg0'] = base64.b64encode(b'/no/such/midnight-restore').decode()
            with snapshot.open('w') as file:
                keys.write(file)
            if args.placements:
                for section in keys.sections():
                    if section.startswith('Window ') and base64.b64decode(keys[section]['role']) == b'normal':
                        keys[section]['x'] = str(args.width + 2000)
                        keys[section]['y'] = str(args.height + 1000)
                with snapshot.open('w') as file:
                    keys.write(file)
            missing_saved = snapshot.read_bytes()
            configure(True, True)
            start()
            partial = client()
            assert partial['id'] == partial['previous'] == original_id
            if args.placements:
                window = handles(partial)['normal']
                until(lambda: '=' in command('xprop','-id',window,'_NET_FRAME_EXTENTS'), 'restored window not managed')
                left, right, top, bottom = map(int, command('xprop','-id',window,'_NET_FRAME_EXTENTS').split('=',1)[1].split(','))
                expected = (args.width - left - right - base['normal'][2],
                            args.height - top - bottom - base['normal'][3], *base['normal'][2:])
                until(lambda: geometry(window) == expected, 'offscreen geometry not recovered to nearest work area')
            os.kill(companion_pid(partial), signal.SIGUSR1)
            time.sleep(.3)
            assert snapshot.read_bytes() == missing_saved, 'startup replaced snapshot before missing client registered'
            stop(partial)
            # With restore disabled, autostart still runs normally and gets a fresh ID.
            configure(False)
            start()
            fresh = client()
            assert fresh['previous'] == '' and fresh['id'] != original_id
            stop(fresh)
            # Corruption is preserved, and ordinary autostart remains eligible.
            snapshot.write_text('malformed: preserve me\n')
            configure(True)
            start()
            corrupt = client()
            assert corrupt['previous'] == ''
            assert snapshot.read_text() == 'malformed: preserve me\n'
            stop(corrupt)
            assert not (root / 'home/should-not-exist').exists(), 'saved command interpreted by shell'
            assert not list((root / 'runtime').glob('midnight-session-*')), 'companion handoff leaked'
            print('Checkpoint/relaunch, saved-ID adoption, autostart deduplication, new bus/agent, literal argv, fresh login, corrupt preservation and cleanup passed.')
        finally:
            if process and process.poll() is None:
                if (root / 'companion.pid').exists():
                    try:
                        os.kill(int((root / 'companion.pid').read_text()), signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                try:
                    process.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            for pid in owned_clients:
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            xvfb.terminate()
            xvfb.wait(timeout=5)


if __name__ == '__main__':
    main()
