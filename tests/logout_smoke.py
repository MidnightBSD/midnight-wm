#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise coordinated logout with real XSMP peers and an isolated desktop."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, window_for_size

REPO = Path(__file__).resolve().parents[1]


def session(root, binary, fixture):
    config = root / 'config/midnight-wm/config.ini'
    config.parent.mkdir(parents=True)
    config.write_text('[Workspace]\ncompositing=false\n[Session]\nsave_application_checkpoints=true\n')
    autostart = root / 'config/autostart'
    autostart.mkdir()
    for name in ('a', 'b'):
        (autostart / (name + '.desktop')).write_text(
            f'[Desktop Entry]\nType=Application\nName={name}\nExec={fixture} {root} {name}\n')
    snapshot = root / 'state/midnight-wm/applications.ini'
    process = None

    def events():
        path = root / 'events'
        return path.read_text().splitlines() if path.exists() else []

    def modes(a, b):
        for name, mode in (('a', a), ('b', b)):
            (root / (name + '.mode')).write_text(mode)
            (root / (name + '.release')).unlink(missing_ok=True)
        (root / 'events').write_text('')

    def start():
        nonlocal process
        (root / 'events').write_text('')
        log = (root / f'session-{time.time_ns()}.log').open('w')
        process = subprocess.Popen([str(binary), '--', sys.executable, '-c',
            'import os,sys; from pathlib import Path; Path(sys.argv[1]).write_text(str(os.getppid())); os.execv(sys.argv[2],[sys.argv[2]])',
            str(root / 'companion.pid'), str(binary.with_name('midnight-wm'))],
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        log.close()
        until(lambda: 'a complete' in events() and 'b complete' in events(), 'XSMP clients not ready')
        until(lambda: window_for_size(1280, 30), 'panel not ready')

    def stop():
        if process:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            process.wait(timeout=10)

    def logout():
        run('xdotool', 'mousemove', '12', '12', 'click', '1')
        time.sleep(.25)
        run('xdotool', 'mousemove', '40', '334', 'click', '1')
        until(lambda: window_for_size(380, 174), 'exit confirmation missing')
        run('xdotool', 'key', 'Return')
        until(lambda: any(line.endswith('shutdown') for line in events()) or
              any(line.endswith('cancelled') for line in events()), 'logout did not start')

    def cancelled(before):
        until(lambda: any(line.endswith('cancelled') for line in events()), 'logout not cancelled')
        assert process.poll() is None, 'cancelled logout ended desktop'
        assert not any(line.endswith('die') for line in events()), 'cancelled logout sent Die'
        assert snapshot.read_bytes() == before, 'cancelled logout replaced snapshot'
        # Event dispatch continues while saves/dialogs are pending or cancelled.
        run('wmctrl', '-s', '1')
        until(lambda: '= 1' in run('xprop', '-root', '_NET_CURRENT_DESKTOP'), 'desktop event loop blocked')
        run('wmctrl', '-s', '0')

    try:
        start()
        os.kill(int((root / 'companion.pid').read_text()), signal.SIGUSR1)
        until(snapshot.exists, 'baseline checkpoint missing')
        before = snapshot.read_bytes()
        modes('cancel', 'normal')
        logout()
        until(lambda: 'a interact' in events(), 'cancel prompt was not granted')
        assert process.poll() is None
        (root / 'a.release').touch()
        cancelled(before)
        until(lambda: 'a complete' in events() and 'b complete' in events(), 'cancelled save did not settle')
        modes('fail', 'normal')
        logout()
        cancelled(before)
        until(lambda: 'a complete' in events() and 'b complete' in events(), 'failed save did not settle')
        # A rejected private snapshot write must cancel before any Die and
        # preserve the last valid checkpoint for a later retry.
        saved = snapshot.with_suffix('.saved')
        snapshot.rename(saved)
        snapshot.mkdir()
        modes('normal', 'normal')
        logout()
        until(lambda: 'a cancelled' in events(), 'failed checkpoint did not cancel logout')
        assert process.poll() is None and not any(line.endswith('die') for line in events())
        assert saved.read_bytes() == before
        snapshot.rmdir()
        saved.rename(snapshot)
        until(lambda: 'a complete' in events() and 'b complete' in events(), 'write failure did not settle')
        modes('prompt', 'prompt')
        logout()
        until(lambda: sum(line.endswith(' interact') for line in events()) == 1, 'first prompt missing')
        time.sleep(.2)
        assert sum(line.endswith(' interact') for line in events()) == 1, 'multiple simultaneous prompts'
        active = next(line.split()[0] for line in events() if line.endswith(' interact'))
        other = 'b' if active == 'a' else 'a'
        run('wmctrl', '-s', '1')
        until(lambda: '= 1' in run('xprop', '-root', '_NET_CURRENT_DESKTOP'), 'save prompt froze desktop')
        (root / (active + '.release')).touch()
        until(lambda: other + ' interact' in events(), 'second prompt not granted')
        assert not any(line.endswith('die') for line in events()), 'Die preceded all saves'
        saved_at = snapshot.stat().st_mtime_ns
        (root / (other + '.release')).touch()
        assert process.wait(timeout=12) == 0, 'successful logout failed'
        assert 'a die' in events() and 'b die' in events(), 'successful clients did not receive Die'
        assert snapshot.stat().st_mtime_ns != saved_at, 'final checkpoint not written'
        stop()
        # Timed-out saves cancel and preserve the prior checkpoint. A second
        # logout must not mistake a late reply for a fresh successful save.
        start()
        modes('hang', 'normal')
        before = snapshot.read_bytes()
        logout()
        cancelled(before)
        until(lambda: 'b complete' in events(), 'responsive peer did not recover')
        modes('normal', 'normal')
        # The hung connection must reconnect; no second shutdown is sent.
        run('xdotool', 'mousemove', '12', '12', 'click', '1')
        time.sleep(.25)
        run('xdotool', 'mousemove', '40', '334', 'click', '1')
        until(lambda: window_for_size(380, 174), 'retry confirmation missing')
        run('xdotool', 'key', 'Return')
        time.sleep(.7)
        assert process.poll() is None and not any(line.endswith('shutdown') for line in events()), 'timed-out peer entered another save'
        assert snapshot.read_bytes() == before
        stop()
        start()
        modes('normal', 'phase2')
        logout()
        assert process.wait(timeout=12) == 0
        assert 'b phase2' in events() and 'b die' in events(), 'phase-2 logout failed'
        stop()
        start()
        modes('normal', 'ignore-die')
        logout()
        assert process.wait(timeout=12) == 0, 'Die acknowledgement timeout blocked logout'
        assert 'b die' in events()
        stop()
        config.write_text('[Workspace]\ncompositing=false\n[Session]\nsave_application_checkpoints=false\n')
        before = snapshot.read_bytes()
        start()
        modes('normal', 'normal')
        logout()
        assert process.wait(timeout=12) == 0
        assert snapshot.read_bytes() == before, 'saving-disabled logout changed checkpoint'
        print('PASS: cancelled/failed/timed-out saves, snapshot preservation, responsive desktop, serialized prompts, final checkpoint, Die and phase-2 logout')
    finally:
        stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=REPO / 'build/midnightbsd-session')
    parser.add_argument('--fixture', type=Path, default=REPO / 'build/xsmp_logout_fixture')
    parser.add_argument('--session', type=Path)
    args = parser.parse_args()
    if args.session:
        session(args.session, args.binary.resolve(), args.fixture.resolve())
        return
    root = Path(tempfile.mkdtemp(prefix='midnight-logout-smoke-'))
    print(f'Artifacts: {root}', flush=True)
    for name in ('home', 'config', 'config-system', 'data', 'data-system', 'state', 'runtime'):
        (root / name).mkdir()
    (root / 'runtime').chmod(0o700)
    env = os.environ.copy()
    for name in ('DBUS_SESSION_BUS_ADDRESS', 'DBUS_SESSION_BUS_PID', 'SESSION_MANAGER', 'ICEAUTHORITY',
                 'SSH_AUTH_SOCK', 'SSH_AGENT_PID'):
        env.pop(name, None)
    env.update(HOME=str(root / 'home'), XDG_CONFIG_HOME=str(root / 'config'),
               XDG_CONFIG_DIRS=str(root / 'config-system'), XDG_DATA_HOME=str(root / 'data'),
               XDG_DATA_DIRS=str(root / 'data-system'), XDG_STATE_HOME=str(root / 'state'),
               XDG_RUNTIME_DIR=str(root / 'runtime'), XDG_CURRENT_DESKTOP='MidnightWorkspace', NO_AT_BRIDGE='1')
    read_fd, write_fd = os.pipe()
    xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1280x720x24', '-nolisten', 'tcp'],
                            pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write_fd)
    try:
        with os.fdopen(read_fd) as stream:
            env['DISPLAY'] = ':' + stream.readline().strip()
        child = subprocess.Popen(['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve()),
            '--session', str(root), '--binary', str(args.binary.resolve()), '--fixture', str(args.fixture.resolve())],
            env=env, start_new_session=True)
        try:
            assert child.wait(timeout=90) == 0, f'logout smoke failed; see {root}'
        finally:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    finally:
        xvfb.terminate()
        xvfb.wait(timeout=5)


if __name__ == '__main__':
    main()
