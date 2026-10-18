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
    args = parser.parse_args()
    args.binary, args.fixture = args.binary.resolve(), args.fixture.resolve()
    output = (args.output or Path(tempfile.mkdtemp(prefix='midnight-restore-results-'))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(f'Results: {output}', flush=True)
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
                         f'Exec={args.fixture} --output "{root / "clients"}" --literal "{literal}"\n')
        env = os.environ.copy()
        for name in ('DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'DBUS_SESSION_BUS_PID', 'SSH_AUTH_SOCK', 'SSH_AGENT_PID',
                     'SESSION_MANAGER', 'ICEAUTHORITY', 'MIDNIGHT_WM_RESTORE_PLAN', 'MIDNIGHT_WM_RESTORE_MANAGER'):
            env.pop(name, None)
        env.update(HOME=str(root / 'home'), XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_CONFIG_DIRS=str(root / 'config-system'), XDG_DATA_HOME=str(root / 'data'),
                   XDG_DATA_DIRS=str(root / 'data-system'), XDG_STATE_HOME=str(root / 'state'),
                   XDG_RUNTIME_DIR=str(root / 'runtime'), XDG_CURRENT_DESKTOP='MidnightWorkspace', NO_AT_BRIDGE='1')
        read_fd, write_fd = os.pipe()
        with (output / 'xvfb.log').open('w') as log:
            xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1280x720x24', '-nolisten', 'tcp'],
                                    pass_fds=(write_fd,), stdout=log, stderr=subprocess.STDOUT)
        os.close(write_fd)
        with os.fdopen(read_fd) as display:
            env['DISPLAY'] = ':' + display.readline().strip()
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
                "from pathlib import Path; import os,sys; Path(sys.argv[1]).write_text(str(os.getppid())); os.execv(sys.argv[2],[sys.argv[2]])",
                str(root / 'companion.pid'), str(args.binary.with_name('midnight-wm'))],
                env=env, cwd=root / 'home', stdout=log, stderr=subprocess.STDOUT)
            log.close()
        def client():
            paths = until(lambda: set((root / 'clients').glob('*.ini')) - known, 'client did not register')
            assert len(paths) == 1, 'duplicate application launch'
            path = paths.pop()
            known.add(path)
            owned_clients.add(int(path.stem))
            return read(path)['Client']
        def companion_pid(info):
            pid = int((root / 'companion.pid').read_text())
            assert pid > 1 and process.poll() is None
            return pid
        def stop(info):
            nonlocal process
            os.kill(companion_pid(info), signal.SIGTERM)
            assert process.wait(timeout=12) == 0, "WM did not exit cleanly after forwarded termination"
            process = None
        try:
            configure(False, True)
            start()
            first = client()
            assert first['previous'] == ''
            os.kill(companion_pid(first), signal.SIGUSR1)
            until(snapshot.is_file, 'checkpoint not saved')
            saved = snapshot.read_bytes()
            original = read(snapshot)
            original_id = base64.b64decode(original['Application 0']['client_id']).decode()
            assert original_id == first['id']
            stop(first)
            configure(True)
            start()
            restored = client()
            assert restored['id'] == restored['previous'] == original_id, 'saved ID not adopted'
            assert restored['literal'] == literal and restored['cwd'] == str(root / 'home')
            assert restored.getboolean('bus_reachable')
            assert restored['midnight_wm_restore_plan'] == restored['midnight_wm_restore_manager'] == ''
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
            missing_saved = snapshot.read_bytes()
            configure(True, True)
            start()
            partial = client()
            assert partial['id'] == partial['previous'] == original_id
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
