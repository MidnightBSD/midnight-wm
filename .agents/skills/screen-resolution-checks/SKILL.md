---
name: screen-resolution-checks
description: Check Midnight Workspace panel, menus, desktop, and folder layout at the five supported X11 resolutions and after a live resize. Use for display sizing or scaling changes in this repository.
---

# Screen resolution checks

Run `python3 .agents/skills/screen-resolution-checks/scripts/check_resolutions.py`
from the repository root after building `build/midnight-wm`. The script uses an
isolated Xvfb display, temporary HOME/XDG directories, and a session bus for
each size. It opens a folder through FileManager1, checks panel and folder
geometry, saves screenshots and window trees, then stops the session. It also
captures a populated folder, launcher menu, and Properties dialog. It attempts
a live 4K-to-720p RandR resize. Xvfb typically exposes only one mode;
in that case the included `screen_resize_event` helper sends a synthetic root
ConfigureNotify and verifies the window manager's internal reflow. The window
manager ignores synthetic root resizes unless `MIDNIGHT_WM_TEST_SYNTHETIC_RESIZE=1`
is set, which the script does only for the live-resize session.

Review the saved screenshots at 1280×720, 1920×1080, 2560×1440, 3440×1440,
and 3840×2160. Check that panel controls and task overflow remain usable;
folder icons, menus, scrollbars, and dialogs are readable; windows remain
reachable after a resize; and desktop shortcuts can be reached by scrolling
when they exceed the screen. Report the output directory and distinguish RandR
from a synthetic resize. For a visual change, open the screenshots
instead of relying only on geometry assertions.

The target sizes are also recorded in `AGENTS.md`. Run this skill for future
layout changes; ordinary non-visual edits do not need this matrix.
For theme changes, repeat with `--config /path/to/config.ini` to test dark mode
and custom fonts in the isolated session. Keep automatic `ui_scale` for the
matrix geometry assertions, and inspect field, selection, and button contrast.

## Multiple monitors

Run `ctest --test-dir build -R display_monitors_test --output-on-failure` for
the monitor selection geometry checks. Xvfb on MidnightBSD reports a single
full-screen RandR monitor even after `xrandr --setmonitor` succeeds, so the
isolated resolution script cannot prove behavior with two real monitors.

For a change to RandR handling, also test on an Xorg session with two outputs:

1. Place a 1920×1080 primary monitor on the left and a 3840×2160 monitor on
   the right. Check that the panel stays within the primary monitor and that
   opening a folder or Properties dialog places it fully on a visible output.
2. Make the right monitor primary without changing the overall framebuffer.
   Check that the panel moves to it. Open a folder and dialog again.
3. Rearrange the outputs with the same framebuffer dimensions, including a
   vertical offset or a gap. Check that no window opens in the gap. Maximize
   and fullscreen an application on each output; each state should use only
   that output. Check that ordinary windows remain reachable.
4. Disconnect and reconnect one output. Check that open windows recover onto
   a visible output, and that the panel and dialogs stay reachable.

Record the output geometry from `xrandr --listmonitors` before and after each
change, plus screenshots or window geometry for any failure. A root-screen
resize alone does not cover monitor rearrangement or a primary-output change.
