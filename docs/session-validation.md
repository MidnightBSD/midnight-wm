# Session save/restore validation

Issue #31 remains open until the remaining hardware checks below are recorded.
The folder, XSMP, Settings, coordinated logout, and opt-in desktop-entry relaunch
phases are implemented. Non-XSMP relaunch is deliberately limited to recognized
installed applications and does not promise document or process restoration.

## Repeatable display checks

Run from a Release build on MidnightBSD 4.2 / Clang 19.1.7:

```sh
ctest --test-dir build --output-on-failure
python3 tests/application_restore_smoke.py --changed-resolution-matrix
python3 tests/session_smoke.py --matrix --binary build/midnightbsd-session
python3 .agents/skills/screen-resolution-checks/scripts/check_resolutions.py
```

The changed-resolution matrix saves on a 3840×2160 Xvfb screen, ends that login,
starts a new X server with the target dimensions, and restores the private
snapshot. It checks recovery of a window saved near the old lower-right edge,
selected workspaces, shaded/maximized/fullscreen state, normal geometry behind
those states, tiny and role-less windows, client IDs, autostart deduplication,
new bus/agent setup, and snapshot preservation. Expected positions account for
the target work area, frame extents, and automatic UI scale. Each case writes
`changed-resolution.txt` with its work area, extents, and expected geometry.
CTest runs the 4K-to-720p case as `application_changed_resolution_smoke`.

The folder matrix checks saving/restoring folders, workspaces, window states,
cancelled logout, malformed snapshots, missing folders, geometry recovery,
Settings controls, and independent clearing. The display skill records populated
folders, menus, and Properties dialogs for visual review.

Results recorded on 2026-10-18: all 38 CTests passed and cppcheck reported no
findings. The additional matrices produced the following results:

| Target screen | XSMP restore from 3840×2160 | Folder session | Display layout |
| --- | --- | --- | --- |
| 1280×720 | Passed | Passed | Passed |
| 1920×1080 | Passed | Passed | Passed |
| 2560×1440 | Passed | Passed | Passed |
| 3440×1440 | Passed | Passed | Passed |
| 3840×2160 | Passed | Passed | Passed |

Menus, populated folders, and Properties screenshots were reviewed at all five
sizes; controls remained reachable. Xvfb provided no smaller live RandR mode.
The skill's live-resize result therefore covers synthetic ConfigureNotify reflow,
while the cross-login matrix covers actual different root-screen sizes. Neither
result proves physical output rearrangement or hotplug.

## Native RandR topology checks

`tests/session_randr_smoke.py` runs a private headless Xorg server with the
X.Org dummy 0.4.x driver. It changes actual RandR outputs while keeping the
3840×1440 framebuffer fixed: primary selection, an offset and gap, output
disable/re-enable, then session restoration with the saved output absent.
Assertions cover panel bounds, named-monitor fullscreen/maximized geometry,
reachable ordinary/shaded windows and built-in folders, restored client identity,
and unchanged application checkpoints. Screenshots and monitor/window listings
are retained in the printed temporary directory.

Build the driver against the installed Xorg SDK without installing it system-wide:

```sh
mkdir -p /tmp/midnight-xorg-dummy
cd /tmp/midnight-xorg-dummy
fetch https://www.x.org/releases/individual/driver/xf86-video-dummy-0.4.1.tar.xz
tar -xf xf86-video-dummy-0.4.1.tar.xz
cd xf86-video-dummy-0.4.1
./configure --prefix=/tmp/midnight-xorg-dummy/install
make
mkdir -p /tmp/midnight-xorg-dummy/modules/drivers
cp src/.libs/dummy_drv.so /tmp/midnight-xorg-dummy/modules/drivers/
```

From the repository root, run:

```sh
python3 tests/session_randr_smoke.py --module-path /tmp/midnight-xorg-dummy/modules
```

To register the optional CTest, configure with
`-DMIDNIGHT_WM_RANDR_MODULE_PATH=/tmp/midnight-xorg-dummy/modules`.
The test requires Xorg, xrandr, xauth, and the usual X11 smoke-test tools.
Use dummy 0.4.x, whose `HW_SKIP_CONSOLE` support avoids console access; the older
0.3.8 mport is unsuitable. The server disables physical input/GPU discovery and
VT switching, uses private authorization with TCP disabled, and never connects
to an inherited display. HOME and XDG directories are isolated.

This test exposed a sole-monitor fallback that incorrectly expanded real output
bounds to the framebuffer after another output was disabled. The fallback now
applies only with the explicit synthetic-resize test flag. The native topology
test passes with the fix and fails at panel recovery without it.

Validation on 2026-10-18: all 40 CTests passed with the optional native test
enabled, cppcheck reported no findings, and the five-size display matrix passed.
Menu, folder and Properties screenshots remained usable at each size. The live
Xvfb check used synthetic ConfigureNotify; the separate native test used actual
RandR events. The Xinerama fullscreen edge/geometry smoke check also passed.
The experimental mport's test dependencies include Xorg, xrandr and xauth;
the optional dummy 0.4.x module remains a separately prepared test fixture.

These are genuine RandR protocol events from virtual outputs. Physical cable
hotplug, hardware drivers, and mixed-DPI displays still require the checks below.

## Remaining Xorg hardware checks

No physical Xorg display was exposed to the automated runner. Use a dedicated test
account on a two-output session and record `xrandr --listmonitors`, screenshots,
and relevant window geometry before and after each change:

1. With a 1920×1080 primary output on the left and a 3840×2160 output on the
   right, save ordinary, maximized, shaded, and fullscreen participating windows
   plus built-in folders on both outputs. Log out and restore them; verify
   workspace assignment, normal geometry after expanding/restoring, and visibility.
2. Change the primary output without changing framebuffer dimensions. Check the
   panel location, folder/dialog placement, and application bounds, then repeat
   a save/login restore cycle.
3. Rearrange outputs with vertical offsets and a gap while retaining framebuffer
   dimensions. Repeat restoration and verify no title bars or dialogs land in
   the gap; fullscreen/maximized windows must use a visible output.
4. Save a session, disconnect its secondary output, then log in again. Confirm
   applications and folders recover within the remaining work area. Also remove
   and reconnect the output during a running session to exercise real RandR
   notification handling.
5. Restore the original layout and record whether normal window geometry and
   named-monitor state remain sensible through another save/login cycle.

Keep issue #31 open until these checks pass or any discovered defects are fixed
and rechecked. The scripted matrix is not a substitute for these results.
