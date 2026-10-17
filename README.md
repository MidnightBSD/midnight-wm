# Midnight Workspace

Midnight Workspace is a Warp-inspired X11 desktop shell and window manager for
MidnightBSD. The current target is MidnightBSD 4.2 with Clang 19 and C++23.

## Build

The build needs CMake, pkg-config, XCB (including icccm, keysyms, XFixes, and
RandR), Cairo, Pango, GLib/GIO, GdkPixbuf, and SQLite 3.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

Select **Midnight Workspace** from an X11 display manager, or run
`exec /usr/local/bin/midnight-wm` from `.xinitrc`. Only one window manager can
control an X screen at a time. `midnight-wm --version` prints the version.

## Desktop behavior

The top bar contains an application launcher, a lock button immediately to its
right, task buttons, a volume control, a system tray, and a clock. The tray
accepts legacy XEmbed icons and D-Bus StatusNotifierItem icons from applications.
StatusNotifier items use icon theme names or embedded pixmaps and support left,
middle, and right clicks plus mouse wheel actions. A session D-Bus is needed for
StatusNotifier items; the legacy XEmbed tray also works without it.
StatusNotifier context menus supplied through DBusMenu open in the Warp-style
popup, including submenus, disabled items, separators, and toggle marks.
The lock button asks a
running `xscreensaver` daemon to lock the screen with `xscreensaver-command -lock`;
if the daemon is unavailable, the desktop shows a notice. Start `xscreensaver`
in your X session to use the button. When windows exceed the available taskbar
space, use the small down-arrow button after the visible tasks to open the
remaining window list.
The list has pages when needed, and selecting a window raises it. Open
**Programs** in the launcher to browse installed X11
applications with their desktop entry icons. The launcher stays open while you
browse Programs pages and closes when you select an item or click outside.
Launcher icons follow the selected freedesktop icon theme, including inherited
themes and the `hicolor` fallback.
Use the speaker buttons marked `−` and `+`, or the mouse wheel over the volume
control, to change output volume in 5% steps. The crossed speaker button sets
both master mixer channels to zero and restores their previous levels when
pressed again during the same session. The control uses MidnightBSD's default
OSS `/dev/mixer` device, falling back to its PCM control
if a master volume control is unavailable. It shows `N/A` when no usable mixer
is available.

Double-click a desktop or folder object to open it. Right-click to see its
actions. Folder windows support keyboard copy, cut, and paste. The shell uses
the standard X11 `CLIPBOARD` selection, and file copy/paste uses
`text/uri-list` plus `x-special/gnome-copied-files` for other file managers.
Folder windows have vertical scrollbars: use the arrows, click the track to
move a page, or drag the thumb. The mouse wheel and Page Up, Page Down, Home,
and End keys also scroll. Other applications draw and control their own
scrollbars.
In a folder window, **File** opens the selected item, creates a folder, shows
properties, moves an item to Trash, or closes the window. **Edit** offers copy,
cut, and paste. **View** refreshes the folder and sorts by name or type.

The `Home`, `Trash`, and `Templates` objects are created on the desktop at first
run. Physical files appear from the user's XDG Desktop directory. Shell object
metadata is stored in `$XDG_DATA_HOME/midnight-wm/workspace.db` (normally under
`~/.local/share`). Files and folders remain in the filesystem. Deleting a file
uses the freedesktop Trash mechanism.

Drag files between folder windows, the desktop, and other XDND-aware file
managers. Clipboard and drag operations support file URIs; text selection uses
standard X11 selection targets, including incremental transfers for large text.
The workspace keeps a text or file clipboard snapshot when its original owner
exits. The context menu provides object properties, shadows, templates, and
Trash restore actions. Copying or moving files runs in the background.

Copy `share/midnight-wm/config.ini` to
`$XDG_CONFIG_HOME/midnight-wm/config.ini` (normally
`~/.config/midnight-wm/config.ini`) to change the panel label, clock format,
and workspace colors. Restart the session after editing it.

At startup, Midnight Workspace launches eligible `.desktop` files from the
user and system XDG autostart directories. A user entry takes precedence over
a system entry with the same filename. `Hidden`, `OnlyShowIn`, `NotShowIn`, and
`TryExec` are respected. Direct `.xinitrc` sessions identify themselves as
`MidnightWorkspace` through `XDG_CURRENT_DESKTOP` when it is not already set.

Alt+Tab switches tasks, and Alt+F4 closes the active window. The title bar
provides minimize, maximize, and close buttons; drag the title bar to move a
window and its lower frame to resize it. The launcher menu has **Exit X11
session…**, which asks for confirmation before closing the window manager. When
started as the Xsession or from `.xinitrc`, that also ends the X11 session.
X11 clients can supply ICCCM size, focus, and transient-window hints. The
window manager also handles EWMH fullscreen, maximize, hidden, skip-taskbar,
frame-extents, and application move/resize requests. Interactive move/resize
supports pointer drags or arrow keys, with Escape to cancel.
The desktop sets an explicit pointer cursor as soon as the window manager starts.
The default silver panel follows the OS/2 Warp 4 references, with a lavender
desktop. Existing user configuration overrides those defaults.
The clickable launcher button uses the MidnightBSD logo from the
[MidnightBSD website art collection](https://github.com/MidnightBSD/www/tree/main/art/logo).

## Design

The shell object model, file operations, and presentation code are separate
from X11 window management and selection handling. This keeps the shell
available for a later Wayland compositor backend. Wayland will require a new
compositor implementation; XCB window IDs and events do not cross that boundary.

The experimental MidnightBSD mport is in
`packaging/mports/x11-wm/midnight-wm` and tracks the published 0.1.0 source
archive. The source tree is now developing 0.1.1; `cpack --config
build/CPackSourceConfig.cmake -G TGZ` generates a 0.1.1 source archive. The
mport remains on 0.1.0 until a 0.1.1 release asset is available.
