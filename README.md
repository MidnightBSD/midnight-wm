# Midnight Workspace

Midnight Workspace is a Warp-inspired X11 desktop shell and window manager for
MidnightBSD. The current target is MidnightBSD 4.2 with Clang 19 and C++23.

## Build

The build needs CMake, pkg-config, XCB (including icccm, keysyms, XFixes, and
RandR), Cairo, Pango, GLib/GIO, and SQLite 3.

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

The top bar contains an application launcher, task buttons, and a clock.
Double-click a desktop or folder object to open it. Right-click to see its
actions. Folder windows support keyboard copy, cut, and paste. The shell uses
the standard X11 `CLIPBOARD` selection, and file copy/paste uses
`text/uri-list` plus `x-special/gnome-copied-files` for other file managers.

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

Alt+Tab switches tasks, and Alt+F4 closes the active window. The title bar
provides minimize, maximize, and close buttons; drag the title bar to move a
window and its lower frame to resize it.

## Design

The shell object model, file operations, and presentation code are separate
from X11 window management and selection handling. This keeps the shell
available for a later Wayland compositor backend. Wayland will require a new
compositor implementation; XCB window IDs and events do not cross that boundary.

The experimental MidnightBSD mport is in
`packaging/mports/x11-wm/midnight-wm`. Its release URL expects a matching
`midnight-wm-0.1.0.tar.gz` source archive; `cpack --config
build/CPackSourceConfig.cmake -G TGZ` generates one.
