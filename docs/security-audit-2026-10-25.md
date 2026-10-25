# Security review — 2026-10-25

Source review of Midnight Workspace at `caacf64` (branch
`feature/system-monitor-hardening`). This is a follow-up to
`security-audit-2026-10-17.md` covering the roughly 10,000 lines added since
baseline `95d708c`: the Midnight Chat companion and its tools, the XSMP session
manager and application restore, keyboard shortcuts, startup applications,
network and Bluetooth panels, the compositor helper, and the System Monitor and
Search companions. The X11 core, shell, clipboard, XDND, and D-Bus services were
re-read in full. No files were changed as part of this review; findings are
recorded here for follow-up work.

Release build, cppcheck, and all 49 CTest cases passed on the review host.

## Trust model

Unchanged from the previous review. The WM runs as the desktop user. Any X11
client on the display can send any ClientMessage, own selections, and name any
window XID in requests; the WM cannot isolate X11 clients from one another but
should not amplify what they can do. Session D-Bus peers can call every exported
service. Files in shared or downloaded locations are hostile. New for this
review: the Chat model server, and any content returned by its tools, are
treated as untrusted (prompt injection).

## Status

Findings 1, 2, 3, 4, 5, 6, 8, 9, 10, and 11 were fixed in the working tree
after this review (see "Fixes applied" at the end). Finding 7 (Chat
web-search confirmation scope) is deliberately left open pending a design
decision. The Low findings remain as recorded.

## Findings

Ordered by severity. Line numbers refer to `caacf64`.

### High

1. **Clipboard INCR replaces the WM's event mask on an attacker-chosen window.**
   `src/clipboard.cpp:322-324` sets `XCB_CW_EVENT_MASK` on `request.requestor`
   to exactly `PROPERTY_CHANGE`. The requestor window in a ConvertSelection is
   chosen by the requesting client and may be any window, including the root
   window or a WM frame. Because every X11 user in this process shares one
   connection, this overwrites *our* event selection on that window. A client
   that makes the WM own more than 64 KiB of CLIPBOARD text (copy, then exit so
   the manager snapshot takes over) and requests it into the root window strips
   `SubstructureRedirect`/`SubstructureNotify` from the root
   (`src/x11.cpp:470-472`): new windows are no longer managed and another WM
   can take over the display. `src/dnd.cpp:443-448` already does this
   correctly by reading `your_event_mask` and OR-ing the bit. Fix: mirror the
   DND code, and reject requestors equal to the root or the clipboard's own
   window.

2. **`_NET_CLOSE_WINDOW` on an unmanaged XID reaches `xcb_kill_client`.**
   `src/x11.cpp:2272` calls `close(e->window)` without checking `frames_`.
   `close()` (`src/x11.cpp:1634-1654`) falls back to `xcb_kill_client` when
   the window lacks `WM_DELETE_WINDOW`. Any client can send the message naming
   a WM frame, the panel, or the `_NET_SUPPORTING_WM_CHECK` window; the server
   then kills the WM's own connection and the session ends. The same message
   with another application's internal window kills that application. Every
   other ClientMessage handler looks the window up in `frames_` first. Fix:
   return early from `close()` when the window is not a managed client.

### Medium

3. **XDND drops are accepted without evidence of a real drag.**
   `src/dnd.cpp:395-420` honours XdndEnter/Position/Drop ClientMessages from
   any client without checking that the named source owns `XdndSelection`,
   that a pointer button is held, or that the pointer is over the target. The
   dropped URI list and copy/move action are then source-chosen
   (`src/shell.cpp:1621-1672`, `src/model.cpp:148-175`), so three forged
   messages aimed at the Trash icon trash arbitrary user-owned paths with no
   interaction, and a move action relocates them. Mitigation: verify the
   selection owner and pointer state before accepting, and confirm drops that
   name paths outside the drop tree or request a move into Trash.

4. **Thumbnail and icon decoding have no dimension limit.**
   `src/thumbnail.cpp:113-116` calls `gdk_pixbuf_get_file_info` with the
   width/height outputs discarded and then decodes with
   `gdk_pixbuf_new_from_file_at_scale` on the main thread. PNG and most other
   loaders allocate the full image before scaling, so a small PNG declaring
   20000×20000 in Documents, Pictures, Downloads, or the Desktop allocates
   about 1.6 GB and stalls the whole desktop while it decodes. The 64 MiB
   on-disk bound does not bound the decoded size. `src/wallpaper.cpp:14-21`
   already has the right guard (`maximum_axis`, `maximum_pixels`); apply it in
   `thumbnail.cpp` and in `image_surface` (`src/shell.cpp:524-549`), and
   consider moving thumbnailing to a worker thread.

5. **`_NET_WM_MOVERESIZE` steals focus and grabs input without user input.**
   `src/x11.cpp:2292-2303` calls `focus()` (bypassing `can_activate()` and
   user-time checks) and `begin_drag()` for any mapped managed window, taking
   pointer and keyboard grabs. A background window can repeatedly refocus
   itself and hold a keyboard grab in keyboard mode until the user presses
   Escape. Fix: for pointer modes verify the claimed button is actually down
   (`xcb_query_pointer`), for keyboard modes require `e->window == active_`,
   and do not call `focus()` unless activation would normally be allowed.

6. **System tray docks arbitrary XIDs, including WM frames.**
   `src/tray.cpp:110-127` rejects only the owner, host, shell-owned windows,
   and INPUT_ONLY windows before reparenting the window into the panel at
   icon size. A REQUEST_DOCK naming another application's frame reparents that
   frame and its client into the tray, leaving stale state in `frames_`.
   `icons_`/`order_` are unbounded. Fix: reject windows the WM manages
   (frames, clients, docks, support windows), require the current parent to
   be the root, and cap docked icons.

7. **Chat web-search confirmation is scoped to a single reply.**
   `src/chat_core.cpp:573` keeps `local_ran` as a local of `complete()`, but
   earlier tool results are replayed into every request
   (`src/chat_core.cpp:168-175`). A prompt-injected model can call
   `file_search` or `system_information` in one reply, end the turn, and in
   the next reply call `web_search` with those results in the query with no
   dialog. `tests/chat_tools_protocol.py:106-108` codifies the current
   behaviour. Fix: derive the flag from the context actually being sent (any
   non-`web_search` tool result in `t.messages` after the compaction point, or
   a flag on the thread set when a summary absorbed one), or offer a
   confirm-every-search option. Related: the first search in a reply never
   confirms, so pasted user text and earlier search snippets are also
   exfiltratable at 256 bytes per call.

8. **FileManager1 opens unbounded folder windows synchronously.**
   `src/file_manager1.cpp:84-91` loops over every URI and
   `Shell::show_folder` always creates a new X window and lists the directory
   on the main thread; `folders.size()` is used only for the cascade offset.
   One `ShowFolders` call with thousands of URIs hangs the WM and exhausts X
   resources. Fix: cap URIs per call, reuse an open window for the same
   normalized path, and cap folder windows globally.

9. **StatusNotifierWatcher accepts unlimited registrations from one peer.**
   `src/status_notifier.cpp:183-220` rejects only exact `service+path`
   duplicates. Each registration adds a signal subscription and a `GetAll`,
   and `src/status_panel.cpp:108-110` grows the tray by one slot per item, so
   a loop of distinct object paths swallows the panel and makes repaint
   O(N²). `hosts` grows the same way. Fix: cap items per sender and overall,
   and cap hosts.

10. **Clipboard incoming fetch has no timeout.**
    `src/clipboard.cpp:165-183` starts a snapshot whenever any client takes
    CLIPBOARD; if the owner never answers, answers with a different target
    (line 371 returns without advancing), or announces INCR and never writes,
    `fetch_`/`snapshotting_` stay set. Paste in the shell then silently does
    nothing and SAVE_TARGETS requests are parked forever. `tick()` only expires
    outgoing transfers. This was listed as remaining work in the previous
    review and is still open. Fix: a per-fetch deadline renewed on each INCR
    chunk, enforced from `tick()`.

11. **Login fails hard if the ICE listener cannot be created.**
    `src/session_main.cpp:84-88` exits with status 1 when
    `SessionManager::initialize()` fails. libICE binds `/tmp/.ICE-unix/<pid>`
    and treats `EADDRINUSE` or a wrongly owned `/tmp/.ICE-unix` as fatal, so
    another local user who pre-creates likely socket names (the directory is
    sticky, so the victim cannot remove them) prevents the victim's graphical
    login. Fix: on failure, log and `execvp` the WM directly as the
    inherited-`SESSION_MANAGER` branch at lines 66-73 already does.

### Low

12. **Inherited `SSH_AUTH_SOCK` is trusted on reachability alone.**
    `src/keyring_session.cpp:137` keeps any connectable path and exports it to
    every launched application and the D-Bus activation environment. Require
    `S_ISSOCK` and `st_uid == getuid()`, as `src/private_state.cpp:28` already
    does for session files. Apply the same to the agent socket parsed from
    gnome-keyring output.

13. **Autostart follows symlinked entries that Settings cannot show.**
    `src/autostart.cpp:50` uses `is_regular_file` (follows links) while
    `src/startup_applications.cpp:24-27` uses `symlink_status`, so a symlinked
    `.desktop` in `~/.config/autostart` runs at login but is invisible and
    undeletable in Settings. Use the same no-follow test in both.

14. **Workspace database is created with umask permissions.**
    `src/store.cpp:55-59` uses `create_directories` and `sqlite3_open`, so
    `~/.local/share/midnight-wm/workspace.db` is typically 0644 while every
    other private file in the project is 0600 in a 0700 directory. Create the
    directory 0700 and chmod the database 0600.

15. **Trash Restore writes wherever `trash::orig-path` points.**
    `src/model.cpp:247-257` moves the item to its recorded original path.
    `.trashinfo` files on removable media are author-controlled, so an
    innocuous-looking item can restore into `~/.ssh` or
    `~/.config/autostart`. `G_FILE_COPY_NONE` prevents overwriting existing
    files. Show the destination before restoring or refuse paths outside the
    trash's own volume.

16. **Externally sourced labels bypass `display_text.hpp`.**
    `sanitize_display_text` is used only by Search and System Monitor. Raw
    strings reach Pango for file names, panel task titles (`_NET_WM_NAME`
    bytes, no UTF-8 validation at `src/x11.cpp:1413-1419`), launcher names,
    recent-file titles, SSIDs, Bluetooth names, MPRIS metadata, notification
    text, dbusmenu labels, FileManager1 paths in notices, and the Chat
    transcript and web-search confirmation dialog (`src/chat.cpp:135-147`,
    `src/chat_core.cpp:602`). Bidi overrides reorder visible text and embedded
    newlines overwrite neighbouring list rows, which are drawn without a clip.
    Route labels through the sanitizer and clip rows. The sanitizer itself
    misses U+00AD, U+200C, U+2061-2064, U+FFF9-FFFB, U+180E, tag characters
    U+E0000-E007F, and variation selectors.

17. **Unauthenticated ICE peers can saturate the connection table.**
    `src/session_manager.cpp:614-629` admits peers before cookie
    authentication, with a 128-connection limit and a 3 s grace. Another local
    user can hold the table so legitimate XSMP clients are refused. Shorten
    the grace, check `LOCAL_PEERCRED` against `getuid()` before accepting, or
    reserve slots for authenticated peers.

18. **`retired` RestartAnyway entries grow for the whole session.**
    `src/session_manager.cpp:665-666` adds an entry on every disconnect and
    nothing removes them; `new_client` (line 436) counts them toward the
    limit, so 128 register/disconnect cycles refuse all further clients and
    every stale command is relaunched at next login. Bound `retired`
    separately and do not count it toward live connections.

19. **Any XSMP client can force a persisted global checkpoint every second.**
    `src/session_manager.cpp:233-238` honours `SaveYourselfRequest(global)`
    with a 1 s limit, re-saving every application and rewriting
    `applications.ini` with two fsyncs. Rate-limit client-initiated global
    saves to tens of seconds.

20. **Peer-chosen bus names are called with auto-start enabled.**
    StatusNotifier `GetAll`/`Activate` and dbusmenu calls use
    `G_DBUS_CALL_FLAGS_NONE`, so registering an activatable well-known name
    makes the shell service-activate it. `src/media_player.cpp` already uses
    `NO_AUTO_START`; do the same here.

21. **Resource limits on other D-Bus surfaces.** MPRIS proxies are bounded
    only by the bus's names-per-connection limit (`src/media_player.cpp:39-56`);
    notification summary/body are stored unbounded and re-laid-out on every
    paint (`src/notifications.cpp:307-321`); the 1024 ScreenSaver inhibitor
    cap is global so one peer can exhaust it for everyone
    (`src/desktop_services.cpp:115-118`). Cap proxies, truncate notification
    text at a UTF-8 boundary, and add a per-sender inhibitor cap.

22. **X11 arithmetic edge cases.** `_NET_MOVERESIZE_WINDOW` passes unclamped
    `int32` coordinates into `configure_frame` (`src/x11.cpp:2282-2290`,
    overflow at 1572/1579/464); `refresh_display_layout` can call `std::clamp`
    with `lo > hi` on a RandR monitor narrower than about 160 px
    (`src/x11.cpp:1918-1920`), which traps under hardened libc++;
    `constrain_size` truncates an `int64` aspect result to `int`
    (`src/x11.cpp:245, 251`); `focus()` recurses per transient child
    (`src/x11.cpp:1235-1240`) where `restack_layers` deliberately does not.
    Clamp inputs and use `std::max(lo, hi)` as the y clamp already does.

23. **Clipboard/DND consume PropertyNotify for foreign windows.**
    `src/clipboard.cpp:459-467` and `src/dnd.cpp:604-612` return true for any
    PropertyNotify matching an outgoing INCR transfer, so a client that
    requests a transfer into `WM_HINTS` or `_NET_WM_STATE` on its own window
    hides those property changes from the WM for the life of the transfer.
    Return false for windows other than the clipboard's own. The clipboard
    `send_payload` also lacks the TARGETS-into-active-INCR guard that DND has
    (`src/clipboard.cpp:346-351` vs `src/dnd.cpp:421-429`).

24. **Thumbnail source checked by path then reopened by path.**
    `src/thumbnail.cpp:91-95` and `:113-115`. In a directory another user can
    write to, the file can become a symlink to a FIFO between `stat` and open,
    blocking the main thread. Open with `O_NONBLOCK|O_NOFOLLOW`, `fstat`, and
    decode from the descriptor.

25. **Chat `search_token` persists while web search is disabled.**
    `src/chat_core.cpp:403-411` writes `token` only when authentication is on
    but always writes `search_token`. Both are plaintext in a 0600 file inside
    a 0700 directory with owner and mode enforced on read and write, which is
    acceptable; clear the search token when the feature is off and consider
    the Secret Service as a future home.

26. **WM-as-XDND-source holds the selection for five minutes if the target
    never finishes.** `src/dnd.cpp:357, 561`. New desktop drags are refused
    until then. A 30-60 s deadline after XdndDrop is enough.

### Informational

- Program lookup for `dbus-run-session`, `gnome-keyring-daemon`, `ssh-agent`,
  `xscreensaver`, and `picom` uses the user's `PATH`. No privilege boundary,
  but preferring the install prefix first would match how
  `MIDNIGHT_WM_SETTINGS_PROGRAM` is handled.
- The pkexec launch for Mport Manager passes `Exec` field codes through
  unexpanded to `g_subprocess_newv` (`src/shell.cpp:3497-3503`). The desktop
  file is root-installed, so this is functional only.
- Clipboard manager semantics re-serve the last snapshot after the owner
  releases CLIPBOARD, including passwords a manager cleared by releasing
  ownership. Honouring `x-kde-passwordManagerHint` is the usual mitigation.
- Protocol-inherent: any peer can close or replace another application's
  notification; any client can cancel another launch's startup feedback;
  `_NET_REQUEST_FRAME_EXTENTS` writes to whatever XID is named.

## Positive checks

- No `system()`, `popen()`, or `g_spawn_command_line_*` anywhere. Every
  launch is argv-based; `g_shell_parse_argv` appears only to locate a
  binary or detect pkexec, never to build a command that is then run by a
  shell. `posix_spawn` (not `spawnp`) prevents ENOEXEC shell retry.
- Chat: only five read-only tools, all off by default; no file read, URL
  fetch, or command execution. libsoup with default TLS verification,
  redirects disabled, 1 MiB response caps, proxy resolver disabled, JSON
  types checked before every access, history fields clamped on load and save,
  no Pango markup or URI launching in the transcript, GTK touched only on the
  main thread.
- Session manager: inet listeners disabled and every listener verified
  `AF_UNIX`; 32-byte `arc4random` cookies written O_EXCL 0600 in an `mkdtemp`
  directory under a validated `XDG_RUNTIME_DIR`; only `Program`,
  `CurrentDirectory`, `RestartCommand`, and `RestartStyleHint` are retained
  and `DiscardCommand`/`ShutdownCommand` are never executed; property counts
  and sizes bounded; clients cannot initiate shutdown.
- Reaper: `PROC_REAP_KILL` is kernel-scoped to the subtree; orphans are only
  waited when `pi_subtree != pi_pid`; no async-unsafe signal handlers.
- `private_state.cpp`: directory opened `O_DIRECTORY|O_NOFOLLOW` with owner
  and 0700 checked by `fstat`; files `O_NOFOLLOW`, size-bounded, written via
  O_EXCL temp, `fsync`, `renameat`.
- D-Bus: all three pixel-buffer paths (notification `image-data`, SNI
  pixmaps, panel re-check) validate width, height, stride, channels, and
  byte length with 64-bit math before indexing; every reply and signal body
  is type-checked; icon names containing `/` are rejected so GdkPixbuf never
  opens an attacker path; inhibitor cookies are bound to the sender and
  removed on disconnect; dbusmenu layout bounded by depth, rows, and label
  length.
- X11: `_NET_WM_ICON` decode bounds total elements, dimensions, and
  per-icon count; struts, cardinals, `_NET_DESKTOP_LAYOUT`,
  `_NET_WM_FULLSCREEN_MONITORS`, and `_NET_RESTACK_WINDOW` are validated as
  described in the previous review; WM_CLASS requires both NULs; synthetic
  root ConfigureNotify is ignored outside tests.
- Files: desktop-entry trust (extension, canonical parent, `lstat`, owner,
  executable bit, no group/other write, `Type=Application`) is rechecked at
  launch and for named actions; GIO copies use `NOFOLLOW_SYMLINKS` and never
  `OVERWRITE`; folder-inside-itself is rejected via `weakly_canonical`;
  rename rejects path separators; SQL uses bound parameters throughout.
- Search and System Monitor: fixed argv with `--`, pattern always begins
  with `*` so a leading `-` cannot be an option, output bounded, every
  external string sanitized; kill(2) re-verifies start time and owner first.
- `display_text.hpp` UTF-8 handling is correct: overlongs, surrogates,
  out-of-range, and truncated sequences become U+FFFD.

## Recommended order of work

1. Clipboard event mask (finding 1) and `_NET_CLOSE_WINDOW` (finding 2):
   both are a few lines and remove session-ending attacks.
2. Thumbnail dimension guard (finding 4) by reusing `valid_dimensions`.
3. Chat cross-turn web-search confirmation (finding 7).
4. `_NET_WM_MOVERESIZE` validation, tray dock predicate, FileManager1 and
   StatusNotifier caps, clipboard fetch deadline (findings 5, 6, 8, 9, 10).
5. Session manager fallback and limits (findings 11, 17, 18, 19).
6. XDND drop verification and Trash restore confirmation (findings 3, 15):
   these need UI decisions.
7. Sweep externally sourced labels through `display_text.hpp` and extend its
   table (finding 16).

Each X11 fix should gain a case in `tests/security_x11_test.cpp` or the EWMH
smoke tests; the chat fix should update `tests/chat_tools_protocol.py`, which
currently asserts the bypass.

## Fixes applied

- **Clipboard (1, 10):** `send_payload` reads the requestor's current
  `your_event_mask` and ORs in PropertyChange, and refuses the root window and
  the clipboard's own window as INCR requestors. Incoming fetches carry a 5 s
  deadline renewed on each INCR chunk; `tick()` fails a stalled step so the
  snapshot sequence completes and parked paste or SAVE_TARGETS requests are
  answered. `tests/security_x11_test.cpp` checks mask preservation, the root
  requestor, and recovery from an owner that never replies.
- **`_NET_CLOSE_WINDOW` (2):** `close()` returns unless the window is a managed
  client. The EWMH smoke fixture sends the message naming a frame, the support
  window, and the root, and verifies the WM survives.
- **`_NET_WM_MOVERESIZE` (5):** keyboard modes require the active window;
  pointer modes require a button actually held (and the named button when
  given) and either the active window or an activation-eligible one. The
  fixture confirms no grab is taken without a button.
- **XDND (3):** a drop is accepted only after an XdndPosition, while the named
  source owns XdndSelection, and with the pointer inside the target; the
  pointer's root position replaces the message coordinates as the drop point.
  The security test forges Enter/Position/Drop without owning the selection
  and confirms refusal, then confirms a genuine owner is accepted.
- **Tray (6):** dock requests pass a window filter wired from `main.cpp`
  (`X11WindowManager::manages`), require a root-parented window, and are
  capped at 64 icons.
- **Images (4):** `safe_image_dimensions` in `wallpaper.hpp` is shared by the
  wallpaper loader, `thumbnail.cpp`, and the shell's `image_surface`; header
  dimensions are checked before any decode.
- **FileManager1 (8):** at most 16 URIs per call (InvalidArgs above); the
  shell reuses an existing window for the same normalized path and caps
  folder windows at 32.
- **StatusNotifier (9):** at most 8 items per owner, 64 items overall, and 16
  hosts.
- **Session (11):** when the ICE listener cannot be created, the session
  companion logs the error and execs the window manager directly instead of
  exiting.
