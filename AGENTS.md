# Repository guidance

Midnight Workspace is an X11 desktop shell and window manager for MidnightBSD.
Target MidnightBSD 4.2, Clang 19, and C++23. Keep new code compatible with that
toolchain and prefer standard C++ facilities where they make ownership and error
handling clearer. The visual direction is OS/2 Warp 4 Workspace Manager.

## Source layout

- `src/main.cpp` owns the XCB connection and dispatches events to clipboard,
  drag and drop, shell, then window manager. Preserve that dispatch order and
  destroy X11 users before disconnecting XCB. `--version` uses the CMake
  project version through `MIDNIGHT_WM_VERSION`. Startup re-execs through
  `dbus-run-session` only if no reachable session bus is available; preserve
  the recursion guard and the wrapper's ownership of the new bus.
- `src/keyring_session.*` initializes the configured Secret Service and SSH
  agent before XDG autostart. Preserve an existing Secret Service owner and a
  reachable inherited `SSH_AUTH_SOCK`; stop only the fallback ssh-agent owned
  by Midnight Workspace.
- `src/x11.*` manages external X11 clients, frames, focus, geometry, EWMH, and
  the root pointer cursor.
- `src/shell.*` draws the panel, desktop, menus, folder windows, scrollbars,
  and properties. Programs reads freedesktop `.menu` layouts through Garcon,
  then falls back to desktop entry Categories; preserve the earlier menu levels
  while browsing a category.
  Built-in desktop folders use XDG user directory paths; Documents and Pictures
  are visible by default, while Music, Videos, and Public Share have config
  switches. Hidden shortcut metadata remains in the workspace database.
  The Midnight menu opens `trash:///` directly. Properties includes a visual
  icon chooser; it stores icon theme names or existing image
  paths in the object model. Double-clicking an editable Properties text field
  selects its value. Icon lookup uses the selected theme, inherited themes,
  and `hicolor`, with GdkPixbuf rendering. Folder labels align with their
  icons; keep label drawing and icon hit areas consistent. The shell's exit
  callback asks the main loop to end the session.
- `src/clipboard.*` implements the X11 `CLIPBOARD` selection and persistence.
- `src/dnd.*` implements XDND file drag and drop. A drop reports completion
  through its callback after the file operation finishes.
- `src/tray.*` owns the X11 system tray selection and embeds legacy XEmbed
  notification icons in the panel.
- `src/status_notifier.*` provides the D-Bus StatusNotifier watcher and host;
  `src/status_panel.*` draws those items in the X11 panel and sends input actions.
- `src/dbus_menu.*` reads application menus over D-Bus;
  `src/dbus_menu_popup.*` draws their rows and submenus in X11.
- `src/audio.*` controls the default OSS mixer and has no X11 dependency.
- `src/model.*` and `src/store.*` own display independent object and file
  behavior. Keep XCB types and event handling out of these files so a future
  Wayland backend can reuse them.
- `src/desktop_launcher.*` reads application desktop entries and only launches
  user-owned executable files directly on the configured Desktop. Recheck trust
  at launch time. `src/thumbnail.*` reads and writes the shared freedesktop
  thumbnail cache for images; keep thumbnail URI and freshness metadata valid.
- `src/notifications.*` implements the session D-Bus notification service and
  draws passive X11 popups only when no other daemon owns the service name.
- `src/file_manager1.*` serves D-Bus folder, item, and properties requests.
  `src/recent_files.*` reads and updates the shared XBEL recent-files history.
- `src/startup_notification.*` tracks launches over the X11 startup protocol;
  `src/browser_launcher.*` selects the XDG browser default and falls back to
  Raphael for the panel browser button;
  `src/terminal_launcher.*` reads XDG terminal preferences and falls back to
  xterm for the panel terminal button;
  `src/xsettings.*` publishes theme and input settings only when its X11
  selection is free. Programs uses Garcon to read freedesktop `.menu` layouts,
  falling back to its built-in categories.
- `data/` contains the Xsession entry, example configuration, and MidnightBSD
  launcher logo sourced from `MidnightBSD/www/art/logo`.
- `packaging/mports/x11-wm/midnight-wm/` contains an experimental mport.

## Build and check

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cppcheck --enable=warning,performance,portability --std=c++20 \
  --suppress=missingIncludeSystem --error-exitcode=1 --quiet \
  '-DMIDNIGHT_WM_VERSION="dev"' src
```

`cppcheck` uses its C++20 parser; the actual project builds as C++23. Its
version macro definition is only for static analysis. Run an isolated Xvfb
session for changes to window management, drawing, menus, properties, icon
selection, clipboard, XDND, or panel audio controls. For audio checks, record
the original mixer levels and restore them after testing mute. Set temporary
`HOME`, `XDG_CONFIG_HOME`, `XDG_CONFIG_DIRS`, and `XDG_DATA_HOME` paths so smoke
tests do not touch the user's desktop or workspace database or launch their
autostart entries. Keep system icon data available when testing the chooser.
Check that a second window manager cannot take over the test screen, and clean
up test processes. For Properties changes, verify the saved object metadata;
for launcher Trash changes, verify that selecting it opens the Trash folder.

For file operations, test copy, move, name collisions, and an unsuccessful
operation. GIO Trash can be unsupported on `/tmp` mounts on MidnightBSD; treat
that as a platform limitation and verify the source file remains intact.

## Packaging and project rules

- Keep the BSD 2-Clause copyright attribution to **Lucas Holt**.
- When changing installed files, update `CMakeLists.txt` and `pkg-plist` together.
- Keep the experimental mport version aligned with the published release and
  set `distinfo` from that release's exact source asset.
- For a release, generate the source archive with
  `cpack --config build/CPackSourceConfig.cmake -G TGZ`. Set `distinfo` from the
  exact archive uploaded to GitHub: rerunning CPack can produce different
  archive bytes from the same source. Validate the remote asset with a clean
  `bmake package`, not only a CMake build or fake install.
- Keep unrelated checkout changes intact. Commit, push, publish releases, or
  change the GitHub repository only when the user requests it.
