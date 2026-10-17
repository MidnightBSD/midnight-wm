# Midnight Workspace

Midnight Workspace is a Warp-inspired X11 desktop shell and window manager for
MidnightBSD. The current target is MidnightBSD 4.2 with Clang 19 and C++23.

## Build

The build needs CMake, pkg-config, XCB (including icccm, keysyms, XFixes, and
RandR), Cairo, Pango, GLib/GIO, GdkPixbuf, Garcon, and SQLite 3.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

Select **Midnight Workspace** from an X11 display manager, or run
`exec /usr/local/bin/midnight-wm` from `.xinitrc`. Only one window manager can
control an X screen at a time. `midnight-wm --version` prints the version.
At startup, Midnight Workspace uses an existing D-Bus session bus when one is
reachable. If none is available and `dbus-run-session` is installed, it starts
a bus for the X11 session and stops it when Midnight Workspace exits. Without
the D-Bus tools, the desktop still starts, but D-Bus integrations are unavailable.
Before starting desktop applications, Midnight Workspace uses the session bus's
configured `org.freedesktop.secrets` provider for password storage, preserving
an existing provider such as KWallet or KeePassXC. If no provider is available,
it starts GNOME Keyring when installed. A working inherited `SSH_AUTH_SOCK` is
preserved; otherwise GNOME Keyring's SSH component is tried, followed by a
session-owned OpenSSH `ssh-agent`. The fallback agent ends with the session.
Keyring unlock at login depends on the display manager's PAM configuration;
starting a keyring here may prompt for its password when first used.

## Desktop behavior

The top bar contains an application launcher, a lock button immediately to its
right, a terminal button, a web browser button, task buttons, a volume control,
a system tray, and a clock. The tray accepts legacy XEmbed icons and D-Bus
StatusNotifierItem icons from applications.
Panel controls, menus, folders, dialogs, and window title bars scale with the
primary monitor height: 1× at 720p and 1080p, 1.25× at 1440p (including ultrawide), and
2× at 4K. Set `ui_scale` in the `[Workspace]` section of `config.ini` to a value
from `0.75` to `3.0` to override this automatic choice. A resolution change
repositions open folders and dialogs within visible monitor bounds. The panel
and desktop shortcuts use the primary monitor; the lavender desktop covers all
monitors. Scroll the
desktop with the mouse wheel or Page Up/Page Down when its icons exceed the
available height. Applications draw their own controls and use their toolkit's
DPI settings.
StatusNotifier items use icon theme names or embedded pixmaps and support left,
middle, and right clicks plus mouse wheel actions. A session D-Bus is needed for
StatusNotifier items; the legacy XEmbed tray also works without it.
StatusNotifier context menus supplied through DBusMenu open in the Warp-style
popup, including submenus, disabled items, separators, and toggle marks.
When no other notification daemon owns `org.freedesktop.Notifications`, Midnight
Workspace provides passive notification popups. They expire automatically or
can be dismissed with a click. An existing notification daemon keeps control.
The lock button asks a
running `xscreensaver` daemon to lock the screen with `xscreensaver-command -lock`;
if the daemon is unavailable, the desktop shows a notice. Start `xscreensaver`
in your X session to use the button. The terminal button opens the terminal
selected by an `xdg-terminals.list` preference, or `$TERMINAL` if set. It opens
xterm when neither is configured.
The browser button opens the configured XDG default web browser. If no browser
default is configured, it opens MidnightBSD's Raphael browser (`www/raphael`).
If Raphael is unavailable, the desktop shows a notice. When windows exceed the
available taskbar space, use the small down-arrow button after the visible tasks
to open the remaining window list.
The list has pages when needed, and selecting a window raises it. Open
**Programs** in the launcher to browse applications using the freedesktop
`applications.menu` layout (including user overrides, merges, and exclusions).
When none is installed, Midnight uses its built-in categories. The menu displays
submenus as named categories and paginates long lists. **Recent Documents**
reads the shared `recently-used.xbel` history; files opened here are recorded
there too.
The launcher and category menus stay open while you browse, then close when you
launch an app or click outside.
The launcher also opens Trash directly, just above **Exit X11 session…**.
Launcher icons follow the selected freedesktop icon theme, including inherited
themes and the `hicolor` fallback.
Launching an application that supports startup notification shows “Starting…”
until its window appears or the launch times out. The XSETTINGS owner publishes
the GTK icon theme, font, DPI, and double-click values to X11 clients, while
leaving an existing settings owner in control.
Use the speaker buttons marked `−` and `+`, or the mouse wheel over the volume
control, to change output volume in 5% steps. The crossed speaker button sets
both master mixer channels to zero and restores their previous levels when
pressed again during the same session. The control uses MidnightBSD's default
OSS `/dev/mixer` device, falling back to its PCM control
if a master volume control is unavailable. It shows `N/A` when no usable mixer
is available.

Double-click a desktop or folder object to open it. Right-click to see its
actions. Files use their shared MIME type for themed icons. **Open With…** lets
you choose an application and optionally save it as the default for that file
type. Images use the shared `$XDG_CACHE_HOME/thumbnails/normal` preview cache.
An application `.desktop` file placed directly on the Desktop can launch when
it is owned by the current user and marked executable. Other `.desktop` files
cannot launch by double-clicking. Folder windows support keyboard copy, cut,
and paste. The shell uses
the standard X11 `CLIPBOARD` selection, and file copy/paste uses
`text/uri-list` plus `x-special/gnome-copied-files` for other file managers.
Folder windows have vertical scrollbars: use the arrows, click the track to
move a page, or drag the thumb. The mouse wheel and Page Up, Page Down, Home,
and End keys also scroll. Other applications draw and control their own
scrollbars.
In a folder window, **File** opens the selected item, creates a folder, shows
properties, moves an item to Trash, or closes the window. **Edit** offers copy,
cut, and paste. **View** refreshes the folder and sorts by name or type.
On a session D-Bus, Midnight also offers `org.freedesktop.FileManager1`, so
applications can open folders, reveal selected files, and show file properties.

The `Home`, `Downloads`, `Documents`, `Pictures`, `Trash`, and `Templates`
shortcuts appear on the desktop by default. Downloads, Documents, Pictures, and
Templates use their configured XDG user directories, falling back to the
matching folder under the home directory when unset. Music, Videos, and Public
Share are available as optional shortcuts. An XDG directory set to the home
directory is hidden, as that setting disables the special directory. Physical
files appear from the user's XDG Desktop directory. Shell object
metadata is stored in `$XDG_DATA_HOME/midnight-wm/workspace.db` (normally under
`~/.local/share`). Files and folders remain in the filesystem. Deleting a file
uses the freedesktop Trash mechanism.
In an object's Properties dialog, double-click Name or Target to select its
text. Use **Choose...** beside Icon to browse previews from the selected icon
theme, search by name, and choose an icon. **Default** restores the built-in
folder or file icon. Existing image paths continue to display.

Drag files between folder windows, the desktop, and other XDND-aware file
managers. Clipboard and drag operations support file URIs; text selection uses
standard X11 selection targets, including incremental transfers for large text.
The workspace keeps a text or file clipboard snapshot when its original owner
exits. The context menu provides object properties, shadows, templates, and
Trash restore actions. Copying or moving files runs in the background.

Copy `share/midnight-wm/config.ini` to
`$XDG_CONFIG_HOME/midnight-wm/config.ini` (normally
`~/.config/midnight-wm/config.ini`) to change the panel label, clock format,
workspace colors, or desktop shortcuts. In its `[Desktop]` section,
`show_documents` and `show_pictures` default to `true`; `show_music`,
`show_videos`, and `show_public_share` default to `false`. Set any of these to
`true` or `false` to show or hide that shortcut. Restart the session after
editing the file. Existing shortcut names, icons, and positions remain in the
workspace database when a shortcut is hidden.

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
`packaging/mports/x11-wm/midnight-wm` and tracks the published 0.1.1 source
archive. The source tree is now developing 0.1.2; `cpack --config
build/CPackSourceConfig.cmake -G TGZ` generates a 0.1.2 source archive. The
mport remains on 0.1.1 until a 0.1.2 release asset is available.
