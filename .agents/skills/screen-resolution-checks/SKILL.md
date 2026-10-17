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
ConfigureNotify and verifies the window manager's internal reflow.

Review the saved screenshots at 1280×720, 1920×1080, 2560×1440, 3440×1440,
and 3840×2160. Check that panel controls and task overflow remain usable;
folder icons, menus, scrollbars, and dialogs are readable; windows remain
reachable after a resize; and desktop shortcuts can be reached by scrolling
when they exceed the screen. Report the output directory and distinguish RandR
from a synthetic resize. For a visual change, open the screenshots
instead of relying only on geometry assertions.

The target sizes are also recorded in `AGENTS.md`. Run this skill for future
layout changes; ordinary non-visual edits do not need this matrix.
