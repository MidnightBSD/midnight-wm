#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise actual portal Settings and GTK open/save/cancel dialogs in isolated Xvfb.
Requires xdg-desktop-portal, xdg-desktop-portal-gtk, PyGObject, Xvfb and xdotool.
"""
import argparse
import contextlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from settings_smoke import run, until, window_for_size, geometry, frame_extents, window_position


def session(args):
    from gi.repository import Gio, GLib
    home = Path(os.environ['HOME'])
    selected = home / 'portal-open.txt'
    selected.write_text('portal integration fixture\n')
    logs = [open(args.output / name, 'w') for name in ('wm.log', 'frontend.log')]
    wm = subprocess.Popen([str(args.binary)], stdout=logs[0], stderr=subprocess.STDOUT)
    frontend = None
    try:
        until(lambda: window_for_size(1280, 30), 'WM did not start')
        frontend = subprocess.Popen(['/usr/local/libexec/xdg-desktop-portal', '--verbose'], stdout=logs[1], stderr=subprocess.STDOUT)
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        def call(interface, method, parameters):
            return bus.call_sync('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop',
                                 interface, method, parameters, None, Gio.DBusCallFlags.NONE, 10000, None)
        def frontend_ready():
            reply = bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                                  'NameHasOwner', GLib.Variant('(s)', ('org.freedesktop.portal.Desktop',)),
                                  None, Gio.DBusCallFlags.NONE, 2000, None)
            return reply.unpack()[0]
        until(frontend_ready, 'portal frontend did not acquire its name')
        def color():
            try:
                return call('org.freedesktop.portal.Settings', 'ReadOne',
                            GLib.Variant('(ss)', ('org.freedesktop.appearance', 'color-scheme'))).unpack()[0]
            except GLib.Error:
                return None
        until(lambda: color() == 2, 'light portal setting unavailable')
        config = Path(os.environ['XDG_CONFIG_HOME']) / 'midnight-wm/config.ini'
        config.write_text('[Workspace]\nui_scale=1.0\ndark_mode=true\naccent_color=#112233\n')
        until(lambda: color() == 1, 'live dark portal setting unavailable')
        accent = call('org.freedesktop.portal.Settings', 'ReadOne',
                      GLib.Variant('(ss)', ('org.freedesktop.appearance', 'accent-color'))).unpack()[0]
        assert all(abs(a-b/255) < 0.0001 for a,b in zip(accent, (17,34,51))), accent
        replies = {}
        def response(_bus, _sender, path, _interface, _signal, parameters):
            replies[path] = parameters.unpack()
        subscription = bus.signal_subscribe('org.freedesktop.portal.Desktop', 'org.freedesktop.portal.Request',
                                            'Response', None, None, Gio.DBusSignalFlags.NONE, response)
        def pump_result(path):
            while GLib.MainContext.default().pending():
                GLib.MainContext.default().iteration(False)
            return replies.get(path)
        def request(method, title, token, extra=None):
            options = {'handle_token': GLib.Variant('s', token), 'modal': GLib.Variant('b', False),
                       'current_folder': GLib.Variant('ay', list(os.fsencode(home) + b'\0'))}
            options.update(extra or {})
            reply = call('org.freedesktop.portal.FileChooser', method, GLib.Variant('(ssa{sv})', ('', title, options)))
            path = reply.unpack()[0]
            def find_window():
                try:
                    return run('xdotool', 'search', '--onlyvisible', '--name', '^' + title + '$').strip().splitlines()[0]
                except (subprocess.CalledProcessError, IndexError):
                    return None
            window = until(find_window, title + ' not shown')
            run('xdotool', 'windowactivate', '--sync', window)
            time.sleep(0.5)
            width, height = geometry(window)
            x, y = window_position(window)
            left, right, top, bottom = frame_extents(window)
            bounds = (x - left, y - top, x + width + right, y + height + bottom)
            assert bounds[0] >= 0 and bounds[1] >= 30 and bounds[2] <= 1280 and bounds[3] <= 720, bounds
            return path, window
        path, window = request('OpenFile', 'Midnight portal open test', 'open_test')
        run('import', '-window', 'root', str(args.output / 'open-dialog.png'))
        run('xdotool', 'key', 'ctrl+l')
        run('xdotool', 'type', '--clearmodifiers', str(selected))
        run('xdotool', 'key', 'Return')
        result = until(lambda: pump_result(path), 'OpenFile did not respond')
        assert result[0] == 0 and result[1].get('uris') == [selected.as_uri()], result
        path, window = request('OpenFile', 'Midnight portal cancel test', 'cancel_test')
        run('xdotool', 'key', 'alt+c')
        result = until(lambda: pump_result(path), 'cancel did not respond')
        assert result[0] == 1, result
        saved = home / 'portal-save.txt'
        path, window = request('SaveFile', 'Midnight portal save test', 'save_test',
                               {'current_name': GLib.Variant('s', saved.name)})
        run('import', '-window', 'root', str(args.output / 'save-dialog.png'))
        run('xdotool', 'key', 'Return')
        result = until(lambda: pump_result(path), 'SaveFile did not respond')
        assert result[0] == 0 and result[1].get('uris') == [saved.as_uri()], result
        assert wm.poll() is None
        bus.signal_unsubscribe(subscription)
        print('Portal frontend: live color/accent, GTK OpenFile URI, cancellation, SaveFile URI passed.')
    finally:
        for process in (frontend, wm):
            if process:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
        for log in logs: log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/midnight-wm'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--session', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    if args.session:
        session(args); return
    for path in ('/usr/local/libexec/xdg-desktop-portal', '/usr/local/libexec/xdg-desktop-portal-gtk'):
        if not Path(path).is_file(): parser.error('required backend missing: ' + path)
    args.output = (args.output or Path(tempfile.mkdtemp(prefix='midnight-portal-results-'))).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parent.parent
    print('Results:', args.output, flush=True)
    with tempfile.TemporaryDirectory(prefix='midnight-portal-') as folder:
        root = Path(folder)
        for directory in ('home', 'config/midnight-wm', 'config/xdg-desktop-portal', 'etc', 'data', 'runtime', 'portals'):
            (root / directory).mkdir(parents=True, exist_ok=True)
        (root / 'runtime').chmod(0o700)
        (root / 'config/midnight-wm/config.ini').write_text('[Workspace]\nui_scale=1.0\n')
        for source in (repo / 'data/midnight.portal', Path('/usr/local/share/xdg-desktop-portal/portals/gtk.portal')):
            shutil.copy(source, root / 'portals' / source.name)
        for dest in ('portals/midnightworkspace-portals.conf', 'config/xdg-desktop-portal/midnightworkspace-portals.conf'):
            shutil.copy(repo / 'data/midnightworkspace-portals.conf', root / dest)
        read_fd, write_fd = os.pipe()
        with open(args.output / 'xvfb.log', 'w') as log:
            xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1280x720x24', '-nolisten', 'tcp'],
                                    pass_fds=(write_fd,), stdout=log, stderr=subprocess.STDOUT)
            os.close(write_fd)
            try:
                with os.fdopen(read_fd) as stream: display = stream.readline().strip()
                if not display: raise RuntimeError('Xvfb failed')
                env = os.environ.copy()
                for key in ('DBUS_SESSION_BUS_ADDRESS', 'DBUS_SESSION_BUS_PID', 'SSH_AUTH_SOCK', 'SSH_AGENT_PID'):
                    env.pop(key, None)
                env.update(DISPLAY=':' + display, HOME=str(root/'home'), XDG_CONFIG_HOME=str(root/'config'),
                           XDG_CONFIG_DIRS=str(root/'etc'), XDG_DATA_HOME=str(root/'data'), XDG_RUNTIME_DIR=str(root/'runtime'),
                           XDG_CURRENT_DESKTOP='MidnightWorkspace', XDG_DESKTOP_PORTAL_DIR=str(root/'portals'), LC_ALL='C')
                process = subprocess.Popen(['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve()), '--session',
                                            '--binary', str(args.binary), '--output', str(args.output)], env=env, start_new_session=True)
                try:
                    if process.wait(timeout=90): raise RuntimeError('Portal checks failed; see ' + str(args.output))
                finally:
                    with contextlib.suppress(ProcessLookupError): os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
            finally:
                xvfb.terminate(); xvfb.wait(timeout=5)

if __name__ == '__main__':
    main()
