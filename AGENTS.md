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
- `src/session_main.cpp` falls back to launching the window manager directly
  when the XSMP listener cannot be created; another local user can occupy
  `/tmp/.ICE-unix` socket names, and that must never cost the login.
- `src/session_reaper.*` makes the WM a procctl(2) reaper for the processes it
  starts. Construct it after the session bus bootstrap and before anything that
  launches processes. Only reap reparented orphans while running (pidinfo
  subtree differs from pid); never wait for a child this process forked, since
  GLib or its owner waits for it. stop() terminates the subtree (TERM, bounded
  grace, KILL) unless `MIDNIGHT_KEEP_SESSION_PROCESSES=1`.
- `src/keyring_session.*` initializes the configured Secret Service and SSH
  agent before XDG autostart. Preserve an existing Secret Service owner and a
  reachable inherited `SSH_AUTH_SOCK`; stop only the fallback ssh-agent owned
  by Midnight Workspace.
- `src/x11.*` manages external X11 clients, frames, focus, geometry, EWMH, and
  the root pointer cursor. RandR monitor changes must refresh layout even when
  the root size stays the same. Keep fullscreen and maximized windows on their
  named monitor where possible, and recover ordinary windows onto a visible
  monitor when an output disappears. Four EWMH workspaces and Show Desktop
  also control built-in folder visibility through Shell::set_workspace.
  Keep ping and synchronized resize waits nonblocking, bounded, and cancelled
  when clients disappear; call wm.tick() from the main loop.
  Call wm.observe_event() before event dispatch so genuine panel/folder input
  updates the focus policy even when the shell consumes the event. Keep input
  timestamps separate from arbitrary property-event server times, compare X11
  timestamps across wraparound, honor zero user-time opt-outs, and preserve
  explicit taskbar/pager activation. Track and release shared user-time helper
  subscriptions without removing a managed client's required event masks.
  Publish attention/urgency and read-only FOCUSED state; clear both attention
  sources on activation. ABOVE/BELOW layers must survive focus changes and keep
  transients above their parents, including active fullscreen families. Update
  ALLOWED_ACTIONS after geometry hints or window state changes. Advertise only
  the EWMH states/actions actually implemented. Every ClientMessage handler
  must look the window up in the managed set first; close() never reaches
  KillClient for an unmanaged XID, and `_NET_WM_MOVERESIZE` requires a held
  button (pointer modes) or the active window (keyboard modes) before
  focusing or grabbing.
  Fullscreen monitor indices use Xinerama ordering, not RandR ordering. Validate
  all four indices and the resulting rectangle before storing or publishing a
  selection. Clear selections after display topology changes and retain normal
  fullscreen geometry restoration. Pager restacking must translate client XIDs
  to root-level frames, validate sibling/mode, preserve focus, and reapply layers
  and transient ancestry.
  Shading clips a mapped client behind its collapsed title bar without changing
  the client's logical size or publishing HIDDEN. Preserve full geometry across
  shading, minimize, workspace switches, and live theme changes; cancel pending
  resize synchronization when changing shade state. Explicit task/Alt+Tab
  activation, fullscreen, and maximize expand shaded windows. Middle-clicking
  the title toggles shading. Reject shading for undecorated fullscreen/splash
  windows and keep ALLOWED_ACTIONS accurate.
  Read the external pager's _NET_DESKTOP_LAYOUT at startup and after root
  property changes. Validate CARDINAL/32 dimensions, orientation, and corner;
  preserve pager ownership and never overwrite the property. Ctrl+Alt arrow
  navigation follows physical grid directions with bounded empty-cell skipping.
- `src/display_monitors.hpp` queries RandR monitor rectangles and chooses a
  visible monitor for a point. The shell uses the primary monitor for its panel
  and desktop shortcuts while keeping the lavender background across the root.
- `src/shell.*` draws the panel, desktop, menus, folder windows, scrollbars,
  and properties. Publish desktop redraws through a complete offscreen X11
  background pixmap so exposure during dragging never reveals an intermediate
  color fill. Release replaced pixmaps and do not repaint the desktop on every
  Expose when its background cache is valid. Programs reads freedesktop `.menu` layouts through Garcon,
  then falls back to desktop entry Categories; preserve the earlier menu levels
  while browsing a category. Render launcher menus fully offscreen before
  mapping them. Keep submenu animation nonblocking, cancel pending hover
  actions and animations on close, and update the main-loop polling interval
  only while a menu transition is active.
  Desktop icons fill top to bottom from the upper left, then continue in the
  next column. Overflow scrolls by columns; keep drawing and hit testing aligned
  after monitor or scale changes. Folder windows retain their row-first layout.
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
- `data/config.ini` documents the `[Workspace]` appearance settings. `dark_mode`
  defaults to light and applies the dark preset to shell controls, frames,
  notifications, and D-Bus menus. `ui_font` and `title_font`
  accept Pango family/style descriptions with an optional 6–32 point size;
  omitting the size preserves each widget's default. `desktop_color` remains
  the lavender `#RRGGBB` background in either mode. When `desktop_image` is
  absent it defaults to the installed `mnbsd_bg_purple.png`, falling back to
  `desktop_color` alone if missing; an explicit empty value disables it.
  `desktop_image` sets a wallpaper path (absolute or `~/...`; relative paths resolve against the
  config file directory first, then user XDG data `backgrounds`, system XDG
  data `backgrounds/midnightbsd` and `backgrounds`, and the installation
  prefix `share/backgrounds/midnightbsd`). The optional MidnightBSD backgrounds
  port installs into `/usr/local/share/backgrounds/midnightbsd`; preserve this
  default search location. `desktop_image_mode` selects `fill` (default,
  centered crop) or `fit` (whole image centered with letterboxing). Supported
  formats are PNG, JPEG, GIF, and SVG through GdkPixbuf; GIF displays its first
  frame. The image repeats independently on each monitor. `desktop_color` shows
  when the image is unset or unreadable and behind transparent or letterboxed
  areas. Optional `panel_color` and `accent_color` override their defaults.
  These settings are read from
  `$XDG_CONFIG_HOME/midnight-wm/config.ini` (normally
  `~/.config/midnight-wm/config.ini`) and apply live after saving. These settings do not change
  controls drawn by application toolkits.
- `src/settings.cpp` implements the GTK 3 `midnightbsd-settings` companion,
  launched from the Midnight menu. Keep font/color/file controls accessible,
  preserve unknown INI keys and comments when saving, and report errors without
  overwriting malformed files. Live reload must retain existing windows and
  refresh shell controls, frame geometry, wallpaper, and desktop shortcuts.
  Test with isolated HOME/XDG directories; never alter the real user config
  during automated checks.
- `src/search.cpp` is the GTK 3 `midnightbsd-search` companion. It runs
  `/usr/bin/locate` without a shell, escapes glob metacharacters, bounds output
  with `-l 20` and a 15 second kill, and passes every shown path through
  `src/display_text.hpp` so controls and bidi formatting in another user's file
  names cannot disguise what a click opens. `display_text_test` covers the
  sanitizer at compile time; run `python3 tests/search_smoke.py` after changes.
- `src/system_monitor.cpp` is the GTK 3 `midnightbsd-system-monitor`
  companion. It only reads kernel state through sysctl, libutil, libgeom, and
  getmntinfo. Every externally sourced string (process names, passwd names,
  mount names, GEOM descriptions) passes through `src/display_text.hpp`.
  Process rows update in place so selection survives refreshes. End Process
  and Force Quit reject non-positive PIDs, confirm with the user, and recheck
  the PID's start time and owner with kinfo_getproc before kill(2). Keep the
  drive list tall enough for at least five drives. Run
  `python3 tests/system_monitor_smoke.py` after changes.
- `src/clipboard.*` implements the X11 `CLIPBOARD` selection and persistence.
  Call clipboard.tick() from the main loop to reclaim stalled outgoing INCR
  transfers and to expire incoming fetches from owners that never answer;
  preserve the aggregate memory and pending-transfer limits. A requestor is
  any window a client names: never replace that window's event mask, only add
  PropertyChange, and refuse the root and the clipboard's own window.
- `src/dnd.*` implements XDND file drag and drop. A drop reports completion
  through its callback after the file operation finishes. XDND ClientMessages
  are unauthenticated: accept a drop only after an XdndPosition, while the
  named source owns XdndSelection, and with the pointer actually over the
  target; use the pointer's position, not the message's, as the drop point.
- `src/tray.*` owns the X11 system tray selection and embeds legacy XEmbed
  notification icons in the panel. Dock requests name arbitrary XIDs: keep the
  window filter from main.cpp (`wm.manages`), require a root-parented window,
  and keep the icon count bounded.
- `src/status_notifier.*` provides the D-Bus StatusNotifier watcher and host;
  `src/status_panel.*` draws those items in the X11 panel and sends input actions.
  Keep one GetAll in flight per item, bound items per owner and overall, and
  repaint the panel from tick().
- One GLib main-context iteration dispatches every ready source, so the main
  loop cannot cap D-Bus work per pass. Keep D-Bus handlers to bookkeeping and
  defer X requests and drawing to the owning tick(), which runs once per pass.
- `src/chat_core.*` and `src/chat.cpp` implement the `midnightbsd-chat` GTK 3
  companion; `src/chat_tools.*` holds its optional tools. Token costs come from
  `estimate_tokens` (three ASCII letters or digits per token, one per other
  byte) scaled by a llama.cpp `/tokenize` sample when that route exists; keep
  estimates conservative and keep `tests/chat_protocol_test.py` in step with
  the estimator. `compaction_target` holds the 80%/60% and 95%/40% policy.
  Summaries fold in UTF-8 safe fragments, condense an oversized summary before
  failing, and record `summary_model`/`summary_context`. When a reply's tool
  calls do not fit, summarize retained turns (reserve = the reply's cost)
  before truncating results; never drop the system prompt or tool pairs. Discovery results are
  cached per server and model for five minutes behind a mutex; call
  `forget_context_limits()` after settings change. Progress callbacks run on
  the worker thread; marshal UI updates with g_main_context_invoke. Linux
  `/proc` parsing goes through the bounded `proc_*` helpers, which have tests.
- `src/dbus_menu.*` reads application menus over D-Bus;
  `src/dbus_menu_popup.*` draws their rows and submenus in X11.
- `src/audio.*` controls the default OSS mixer and has no X11 dependency.
  The panel has one speaker button toggling a popup below it, with a volume
  slider, mute beneath it, and MPRIS controls. Keep popup bounds within the
  primary monitor; release pointer/keyboard grabs on close and layout changes.
- `src/battery.*` reads aggregate ACPI battery status through MidnightBSD sysctls,
  following `/usr/src/usr.bin/batt`. Keep it independent of X11 and shell commands.
  The panel shows battery capacity beside audio only when a battery is present,
  polls every 30 seconds, and preserves task overflow and tray space. Handle
  unknown capacity/time without inventing values and hide removed batteries.
  `battery_test` covers the reader with injected sysctl values; run
  `python3 tests/battery_smoke.py` for isolated presence, charging, live updates,
  and removal checks through the test-only sysctl interposer.
- `src/media_player.*` discovers MPRIS players asynchronously and respects their
  playback capabilities. Cancel outstanding calls safely when shutting down.
- `src/desktop_services.*` provides the portal Settings backend and idle
  inhibition. Preserve existing ScreenSaver service owners, associate inhibitor
  cookies with their D-Bus callers, and remove them on disconnect. Never bypass
  explicit screen locking. The portal configuration selects the GTK chooser
  backend and respects user overrides.
- `src/model.*` and `src/store.*` own display independent object and file
  behavior. Keep XCB types and event handling out of these files so a future
  Wayland backend can reuse them.
- `src/desktop_launcher.*` reads application desktop entries and only launches
  user-owned executable files without group/other write permission directly on
  the configured Desktop. Recheck trust
  at launch time, including named desktop-entry actions. Installed Programs
  actions use GDesktopAppInfo; never expand Exec with a shell. `src/thumbnail.*` reads and writes the shared freedesktop
  thumbnail cache for images; keep thumbnail URI and freshness metadata valid.
  Check header dimensions with `safe_image_dimensions` (wallpaper.hpp) before
  decoding any untrusted image, including icons loaded by path in the shell.
- `src/notifications.*` implements the session D-Bus notification service and
  draws X11 popups only when no other daemon owns the service name. Preserve
  action identifiers, close reasons, resident/urgency behavior, and validate raw
  image dimensions/stride/payload before creating image buffers. Notify only
  updates popup state; tick() creates, paints, and maps changed popups.
- `src/file_manager1.*` serves D-Bus folder, item, and properties requests.
  Bound URIs per call; the shell reuses an open window for the same folder
  path and caps the number of folder windows.
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
For shell layout and scale changes, check 1280×720, 1920×1080, 2560×1440,
3440×1440, and 3840×2160, including a live resize from a larger size to a
smaller one. Keep panel controls, menus, folder windows, dialogs, and the
desktop usable at each size.
Use `.agents/skills/screen-resolution-checks/SKILL.md` for the repeatable
isolated display matrix and its manual two-monitor Xorg checklist.
For settings UI or live configuration changes, run
`python3 tests/settings_smoke.py` after building. It checks menu launch, Apply,
INI preservation, live color/scale changes, open-folder preservation, and
malformed-file recovery in an isolated session.
For desktop repaint changes, run `python3 tests/desktop_repaint_smoke.py`;
it uncovers wallpaper and a live-updated solid background while the WM is
suspended, verifying that Xorg restores the complete cached desktop.
For launcher menu changes, run `python3 tests/menu_smoke.py`; it checks
category navigation, popup reuse, application launch, and dismissal during reveal.
For compatibility changes, run `python3 tests/notifications_smoke.py`,
`python3 tests/ewmh_smoke.py`, and `python3 tests/media_smoke.py` as applicable.
Run `python3 tests/ewmh_attention.py` for focus timestamps, helper lifecycle,
attention/urgency, stacking layers, allowed actions, and the visible task marker.
It also verifies shade geometry, title-bar input, client mapping, state
transitions, and task activation expanding a shaded window. Run
`python3 tests/pager_smoke.py` for pager startup/live/deleted layouts and real
Ctrl+Alt arrow input; `desktop_layout_test` covers grid math and malformed
dimensions at compile time in Release builds.
Run `python3 tests/ewmh_attention.py --multi-monitor` for an isolated two-screen
Xinerama test of monitor index ordering, live fullscreen selection, mixed edges,
invalid requests, and geometry restoration. Xvfb screens overlap at the origin;
this does not replace real Xorg hotplug and offset-output checks.
`x11_timestamp_test` checks wraparound at compile time, including Release builds.
The media test restores original OSS mixer levels after slider/mute checks.
Desktop services protocol checks are registered with CTest.
With xdg-desktop-portal-gtk installed, run `python3 tests/portal_smoke.py`
to check live appearance settings and actual GTK Open/Save/Cancel requests
through the portal frontend, including dialog bounds at 720p.
Before pushing, use `.agents/skills/pre-push-review/SKILL.md` to run cppcheck
and review the outgoing diff for basic security issues.

Security review findings and remaining hardening work are recorded in
`docs/security-audit-2026-10-17.md`. With Python 3 and Xvfb available, CTest
includes hostile WM_CLASS and bounded clipboard transfer checks. The EWMH
smoke test also checks all frame resize edges, focus recovery, SIGPIPE, and
keymap changes; the settings smoke checks folder mouse buttons and live icon
theme invalidation.

For file operations, test copy, move, name collisions, and an unsuccessful
operation. GIO Trash can be unsupported on `/tmp` mounts on MidnightBSD; treat
that as a platform limitation and verify the source file remains intact.

## Packaging and project rules

- Keep the BSD 2-Clause copyright attribution to **Lucas Holt**.
- When changing installed files, update `CMakeLists.txt` and `pkg-plist` together.
- Keep the experimental mport version aligned with the published release and
  set `distinfo` from that release's exact source asset.
- Keep the experimental mport in sync with `x11-wm/midnight-wm` in
  MidnightBSD/mports. Copy dependency, `USES`, option, and plist changes made
  there (for example the runtime `dbus-run-session`, portal, XScreenSaver, and
  `desktop-file-utils` entries) instead of rebuilding the files from scratch.
- `project(... VERSION ...)` in `CMakeLists.txt` names the next release on
  `master`; the experimental mport keeps the last published version.
- For a release, generate the source archive with
  `cpack --config build/CPackSourceConfig.cmake -G TGZ`. Set `distinfo` from the
  exact archive uploaded to GitHub: rerunning CPack can produce different
  archive bytes from the same source. Validate the remote asset with a clean
  `bmake package`, not only a CMake build or fake install.
- Keep unrelated checkout changes intact. Commit, push, publish releases, or
  change the GitHub repository only when the user requests it.
- When the user asks to commit, push, or submit a fix or improvement, do it
  through a pull request: commit on a new topic branch, push that branch, and
  open a PR against `master`. Never commit, merge, or push directly to
  `master`. Without such a request, leave the change uncommitted in the
  working tree and report it.
