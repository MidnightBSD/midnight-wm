#!/usr/bin/env python3
# Copyright (c) 2026 Lucas Holt
# SPDX-License-Identifier: BSD-2-Clause
"""Check live GTK 3 theme propagation; requires PyGObject and settings smoke tools."""

import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
import settings_smoke as smoke


def session(args):
    config = Path(os.environ["XDG_CONFIG_HOME"]) / "midnight-wm/config.ini"
    gtk_config = Path(os.environ["XDG_CONFIG_HOME"]) / "gtk-3.0/settings.ini"
    original = "[Settings]\ngtk-theme-name=HighContrast\n"
    gtk_config.write_text(original)
    config.write_text("[Workspace]\ndark_mode=true\ncompositing=false\n")
    with (args.output / "wm.log").open("w") as log:
        wm = subprocess.Popen([str(args.binary)], stdout=log, stderr=subprocess.STDOUT)
        try:
            smoke.until(lambda: smoke.window_for_size(1280, 30), "panel did not appear")
            Gtk.init([])
            settings = Gtk.Settings.get_default()

            def theme_is(name):
                while Gtk.events_pending():
                    Gtk.main_iteration_do(False)
                return settings.get_property("gtk-theme-name") == name

            smoke.until(lambda: theme_is("Adwaita-dark"), "GTK missed initial dark theme")
            # Keep the same GTK connection and settings object for every transition.
            for dark, expected in ((False, "HighContrast"), (True, "Adwaita-dark")):
                smoke.atomic_write(config, f"[Workspace]\ndark_mode={str(dark).lower()}\ncompositing=false\n")
                smoke.until(lambda: theme_is(expected), f"GTK did not switch to {expected}")
            gtk_config.write_text("[Settings]\ngtk-theme-name=Adwaita\n")
            smoke.atomic_write(config, "[Workspace]\ndark_mode=false\ncompositing=false\n")
            smoke.until(lambda: theme_is("Adwaita"), "GTK did not restore updated user theme")
            gtk_config.write_text(original)
            smoke.until(lambda: theme_is("HighContrast"), "GTK missed live user theme edit")
            assert gtk_config.read_text() == original, "WM rewrote GTK preferences"
            # A second WM must fail without taking over this screen.
            second = subprocess.run([str(args.binary)], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=10)
            assert second.returncode != 0, "second WM took over"
            assert wm.poll() is None, "original WM exited"
            print("GTK initial dark theme, live toggles, configured-theme restoration, "
                  "live GTK preference edits, and second-WM rejection passed.")
        finally:
            wm.terminate()
            try:
                wm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                wm.kill()
                wm.wait(timeout=5)


if __name__ == "__main__":
    # Reuse the disposable HOME/XDG, Xvfb, D-Bus and process-cleanup harness.
    smoke.session = session
    smoke.main(session_script=__file__)
