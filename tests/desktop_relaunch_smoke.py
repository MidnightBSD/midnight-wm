#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Bounded, opt-in non-XSMP relaunch through installed desktop IDs."""
import argparse
import base64
import configparser
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def until(check, message, timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = check()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/midnightbsd-session'))
    parser.add_argument('--fixture', type=Path, default=Path('build/desktop_relaunch_fixture'))
    args = parser.parse_args()
    binary, fixture = args.binary.resolve(), args.fixture.resolve()
    root = Path(tempfile.mkdtemp(prefix='midnight-desktop-relaunch-'))
    print(f'Artifacts: {root}', flush=True)
    for name in ('home', 'config/midnight-wm', 'config-system', 'data/applications', 'data-system', 'state', 'runtime', 'clients'):
        (root / name).mkdir(parents=True, exist_ok=True)
    (root / 'runtime').chmod(0o700)
    env = os.environ.copy()
    for name in ('DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'DBUS_SESSION_BUS_PID', 'SESSION_MANAGER', 'ICEAUTHORITY',
                 'SSH_AUTH_SOCK', 'SSH_AGENT_PID', 'MIDNIGHT_WM_RESTORE_PLAN', 'MIDNIGHT_WM_RESTORE_MANAGER'):
        env.pop(name, None)
    env.update(HOME=str(root / 'home'), XDG_CONFIG_HOME=str(root / 'config'), XDG_CONFIG_DIRS=str(root / 'config-system'),
        XDG_DATA_HOME=str(root / 'data'), XDG_DATA_DIRS=str(root / 'data-system'), XDG_STATE_HOME=str(root / 'state'),
        XDG_RUNTIME_DIR=str(root / 'runtime'), XDG_CURRENT_DESKTOP='MidnightWorkspace', NO_AT_BRIDGE='1')
    config = root / 'config/midnight-wm/config.ini'
    snapshot = root / 'state/midnight-wm/applications.ini'
    process = None
    children = []
    known = set()
    restored = set()
    read_fd, write_fd = os.pipe()
    xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1280x720x24', '-nolisten', 'tcp'],
        pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write_fd)
    with os.fdopen(read_fd) as stream:
        env['DISPLAY'] = ':' + stream.readline().strip()

    def command(*arguments):
        return subprocess.check_output(arguments, env=env, text=True, timeout=10)

    def configure(restore=False, fallback=False):
        config.write_text('[Workspace]\ncompositing=false\n[Session]\nsave_application_checkpoints=true\n'
            f'restore_applications={str(restore).lower()}\nrestore_desktop_applications={str(fallback).lower()}\n')

    def start():
        nonlocal process
        log = (root / f'session-{time.time_ns()}.log').open('w')
        process = subprocess.Popen([str(binary), '--', sys.executable, '-c',
            'import os,sys; from pathlib import Path; Path(sys.argv[1]).write_text(str(os.getppid())); os.execv(sys.argv[2],[sys.argv[2]])',
            str(root / 'companion.pid'), str(binary.with_name('midnight-wm'))], env=env,
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        log.close()
        until(lambda: '_NET_SUPPORTING_WM_CHECK(WINDOW): window id # 0x' in command('xprop', '-root', '_NET_SUPPORTING_WM_CHECK'), 'WM missing')
        time.sleep(.4)

    def stop():
        if process:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            process.wait(timeout=10)
        for child in children:
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=5)
        children.clear()
        for pid in restored:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        restored.clear()

    def read():
        keys = configparser.ConfigParser(interpolation=None)
        keys.read(snapshot)
        return keys

    def checkpoint():
        previous = snapshot.stat().st_mtime_ns if snapshot.exists() else 0
        os.kill(int((root / 'companion.pid').read_text()), signal.SIGUSR1)
        until(lambda: snapshot.exists() and snapshot.stat().st_mtime_ns != previous, 'checkpoint not updated')
        return read()

    def entry(name, extra=''):
        path = root / 'data/applications' / (name + '.desktop')
        path.write_text(f'[Desktop Entry]\nType=Application\nName=Reopen test\nExec={fixture} {root / "clients"} {name} restored\n{extra}')
        return path

    try:
        application = entry('org.example.Reopen')
        entry('org.example.Hidden', 'Hidden=true\n')
        entry('org.example.Session')
        configure()
        start()
        for name, mode in (('org.example.Reopen', 'multiple'), ('org.example.Hidden', 'normal'),
                           ('org.example.Missing', 'normal'), ('org.example.WrongType', 'wrong-type'),
                           ('org.example.Session', 'xsmp'), ('../bad', 'normal')):
            children.append(subprocess.Popen([str(fixture), str(root / 'clients'), name, mode], env=env))
        until(lambda: len(list((root / 'clients').glob('*.txt'))) == 6, 'fixture did not start')
        until(lambda: len(command('wmctrl', '-l').splitlines()) >= 7, 'fixture windows not managed')
        assert checkpoint()['Applications']['desktop_entries'] == '0', 'fallback capture enabled by default'
        configure(fallback=True)
        time.sleep(3)
        keys = checkpoint()
        assert keys['Applications']['count'] == '0', 'non-XSMP app became an XSMP client'
        assert keys['Applications']['desktop_entries'] == '1'
        assert base64.b64decode(keys['Desktop Applications']['entry0']).decode() == 'org.example.Reopen.desktop'
        assert 'private-command' not in snapshot.read_text() and 'WM_COMMAND' not in snapshot.read_text()
        known.update((root / 'clients').glob('*.txt'))
        before = snapshot.read_bytes()
        stop()
        configure(restore=True)
        start()
        time.sleep(.8)
        assert set((root / 'clients').glob('*.txt')) == known, 'fallback restored without separate opt-in'
        stop()
        assert snapshot.read_bytes() == before
        # A successful restore suppresses the unique matching autostart entry.
        autostart = root / 'config/autostart/reopen.desktop'
        autostart.parent.mkdir()
        autostart.write_text(application.read_text())
        configure(restore=True, fallback=True)
        start()
        new = until(lambda: set((root / 'clients').glob('*.txt')) - known, 'desktop entry not relaunched')
        time.sleep(.5)
        new = set((root / 'clients').glob('*.txt')) - known
        restored.update(int(path.stem) for path in new)
        assert len(new) == 1, 'multiple windows or autostart caused duplicate relaunch'
        info = new.pop().read_text()
        assert info.startswith('org.example.Reopen\nrestored\n'), 'original arguments copied'
        assert all(line.endswith('=') for line in info.splitlines()[2:]), 'private handoff leaked'
        known.update((root / 'clients').glob('*.txt'))
        stop()
        application.unlink()
        autostart.unlink()
        start()
        time.sleep(.8)
        assert set((root / 'clients').glob('*.txt')) == known, 'removed desktop entry relaunched'
        stop()
        print('PASS: separate opt-in, bounded identities, XSMP exclusion, hidden/missing/malformed entries, one launch per application, autostart deduplication, no saved command arguments, handoff cleanup')
    finally:
        stop()
        xvfb.terminate()
        xvfb.wait(timeout=5)


if __name__ == '__main__':
    main()
