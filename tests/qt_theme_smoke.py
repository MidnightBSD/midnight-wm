#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check Qt 6 autostart inheritance and live palettes in isolated Xvfb.

Requires a C++ compiler, Qt6Widgets/Qt6DBus pkg-config metadata, the Qt GTK plugin,
and the settings_smoke.py tools. Does not add a Qt build dependency to the WM.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
import settings_smoke as smoke


def session(args):
    fixture = args.output / 'qt-theme-fixture'
    subprocess.run(['c++', '-std=c++17', '-fPIC',
                    str(Path(__file__).with_name('qt_theme_fixture.cpp')), '-o', str(fixture),
                    *shlex.split(smoke.run('pkg-config', '--cflags', '--libs', 'Qt6Widgets', 'Qt6DBus'))],
                   check=True, timeout=30)
    config = Path(os.environ['XDG_CONFIG_HOME'])
    autostart = config / 'autostart'
    autostart.mkdir()
    gtk = config / 'gtk-3.0/settings.ini'
    gtk.write_text('[Settings]\ngtk-theme-name=Adwaita\n')
    wmconfig = config / 'midnight-wm/config.ini'
    services = Path(os.environ['XDG_DATA_HOME']) / 'dbus-1/services'
    services.mkdir(parents=True)
    for program in (args.binary, args.binary.parent / 'midnightbsd-session'):
        for override in (None, 'generic', ''):
            label = program.name + '-' + ('default' if override is None else override or 'empty')
            result = args.output / (label + '.txt')
            # Desktop Exec quoting is distinct from shell quoting.
            def quote(value):
                return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$') + '"'
            (autostart / 'qt-theme.desktop').write_text(
                '[Desktop Entry]\nType=Application\nName=Qt theme fixture\nExec=' +
                quote(fixture) + ' ' + quote(result) + '\n')
            smoke.atomic_write(wmconfig, '[Workspace]\ndark_mode=true\ncompositing=false\n')
            env = os.environ.copy()
            for key in ('QT_QPA_PLATFORMTHEME', 'QT_STYLE_OVERRIDE', 'GTK_THEME', 'SESSION_MANAGER'):
                env.pop(key, None)
            env['QT_QPA_PLATFORM'] = 'xcb'
            if override is not None:
                env['QT_QPA_PLATFORMTHEME'] = override
            with (args.output / (label + '.log')).open('w') as log:
                wm = subprocess.Popen([str(program)], env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    def latest():
                        lines = result.read_text().splitlines() if result.exists() else []
                        return lines[-1].split('|') if lines else None
                    row = smoke.until(latest, 'Qt autostart did not run: ' + label)
                    assert row[0] == ('gtk3' if override is None else override), row
                    if override is None:
                        def is_dark(dark):
                            row = latest()
                            if not row or len(row) != 4:
                                return False
                            background = sum(bytes.fromhex(row[2][1:]))
                            foreground = sum(bytes.fromhex(row[3][1:]))
                            return row[1] == ('2' if dark else '1') and ((background < foreground) == dark)
                        smoke.until(lambda: is_dark(True), 'initial Qt palette was not dark')
                        for dark in (False, True):
                            smoke.atomic_write(wmconfig,
                                f'[Workspace]\ndark_mode={str(dark).lower()}\ncompositing=false\n')
                            smoke.until(lambda: is_dark(dark), 'Qt palette did not follow live setting')
                    # Activate a fresh process through the pre-existing bus, whose
                    # environment cannot inherit changes made in the WM process.
                    bus_result = args.output / (label + '-dbus.txt')
                    service = 'org.midnight.Test.QtTheme.' + label.replace('-', '_')
                    (services / (service + '.service')).write_text(
                        '[D-BUS Service]\nName=' + service + '\nExec=' +
                        quote(fixture) + ' ' + quote(bus_result) + ' ' + service + '\n')
                    smoke.run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus',
                              '--object-path', '/org/freedesktop/DBus', '--method',
                              'org.freedesktop.DBus.ReloadConfig')
                    smoke.run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus',
                              '--object-path', '/org/freedesktop/DBus', '--method',
                              'org.freedesktop.DBus.StartServiceByName', service, '0')
                    smoke.until(lambda: bus_result.exists() and bus_result.read_text().strip(),
                                'D-Bus Qt fixture did not report')
                    assert bus_result.read_text().split('|')[0] == row[0], bus_result.read_text()
                    # Wait for the short-lived service to exit before the next WM.
                    smoke.until(lambda: 'false' in smoke.run('gdbus', 'call', '--session',
                                '--dest', 'org.freedesktop.DBus', '--object-path',
                                '/org/freedesktop/DBus', '--method',
                                'org.freedesktop.DBus.NameHasOwner', service), 'Qt service did not exit')
                    print(label + ': autostart, D-Bus inheritance and palette checks passed', flush=True)
                finally:
                    wm.terminate()
                    try:
                        wm.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        wm.kill()
                        wm.wait(timeout=5)
            smoke.until(lambda: smoke.window_for_size(1280, 30) is None, 'WM did not exit')


if __name__ == '__main__':
    smoke.session = session
    smoke.main(session_script=__file__)
