# Security review — 2026-10-17

Reviewed the generated Midnight Workspace code at baseline `95d708c`, with
local fixes described below. This is a source review and targeted regression
exercise, not a certification or a complete dependency audit.

## Scope and trust model

Reviewed application launching and desktop-entry trust, file copy/move and
persistence, clipboard and XDND handling, X11 properties and geometry, D-Bus
notifications, menus, tray items, media players, portal settings, idle
inhibition, keyring startup, and configuration writes. Ran cppcheck and the
project tests, plus targeted hostile-input checks on an isolated Xvfb display.

The WM runs with the desktop user's privileges. X11 clients share access to
the session; this WM cannot provide isolation between hostile X11 clients.
Session D-Bus clients can invoke the public desktop services. Path operations
may involve downloaded content, links, and directories shared with other users.

## Findings addressed

1. **Medium: recursive copy into its own source.** The transfer check explicitly
   excluded `.` from rejection. Pasting a folder inside itself could recursively
   create directories and exhaust resources. Direct template copies also lacked
   containment checks. Both entry points now reject the source itself and its
   descendants, including destinations reached through directory links.

2. **Medium: symlink copy and destination confusion.** GIO copies followed file
   symlinks, potentially copying a linked private file into a shared destination.
   Recursive copies accepted an already existing destination directory, including
   a link to another directory. Copies and moves now request link preservation;
   recursive copies require a newly created destination and refuse an existing
   directory or link. This does not eliminate concurrent path replacement races.

3. **Medium: unbounded outgoing clipboard retention.** Each unfinished INCR
   request retained another complete payload without an aggregate limit.
   Outgoing transfers now have a 32 MiB aggregate payload budget, a maximum of
   16 pending transfers, duplicate requestor/property rejection, and a 30 second
   inactivity deadline. Excess requests fail through normal SelectionNotify.
   Main-loop ticks reclaim expired payloads. Tests cover exhaustion and recovery.

4. **Medium, conditional on shared write permissions: writable desktop
   launchers.** Owner and execute checks accepted group- or world-writable
   entries. Another user with write access could change the command while
   retaining the launcher's trusted status. Such modes now fail trust checks
   for both the main entry and its actions. Tests cover both modes.

5. **Low: invalid view length from malformed WM_CLASS.** A missing NUL separator
   used `std::string::npos` as a string_view length, violating its valid-range
   requirement. The view is now bounded to the actual property length. The
   current equality checks do not establish an exploitable read; the original
   code nevertheless constructed an invalid view. A real X11 fixture tests the
   unterminated property while startup tracking is active.

6. **Low: signed overflow in resize increment arithmetic.** Negative client
   base sizes, including INT_MIN, could overflow `width - base_width` or its
   height equivalent. Bases are now clamped to the nonnegative X11 size range
   before arithmetic. The EWMH fixture includes hostile base sizes.

## GitHub issues reviewed

All eight open reports were checked against the baseline and addressed locally:

| Issue | Assessment and change |
| --- | --- |
| [#1](https://github.com/MidnightBSD/midnight-wm/issues/1) | Confirmed UI bug. Frame hit testing chooses each of the eight resize directions; titlebar interiors still move. |
| [#2](https://github.com/MidnightBSD/midnight-wm/issues/2) | Confirmed memory leak. Free GLib's allocated UUID after copying it. |
| [#3](https://github.com/MidnightBSD/midnight-wm/issues/3) | Confirmed service ownership bug. Unregister Notifications and FileManager1 objects on RequestName failure or refusal, so unique-name calls cannot reach inactive services. |
| [#4](https://github.com/MidnightBSD/midnight-wm/issues/4) | Valid defensive change. Ignore SIGPIPE at startup so a broken pipe can be handled as an I/O error. No concrete socket-triggered crash was demonstrated. |
| [#5](https://github.com/MidnightBSD/midnight-wm/issues/5) | Confirmed focus bug. Prefer a visible transient parent, otherwise focus the topmost eligible visible client using actual stacking order. |
| [#6](https://github.com/MidnightBSD/midnight-wm/issues/6) | Confirmed UI bug. Only a left click activates the built-in folder close control. |
| [#7](https://github.com/MidnightBSD/midnight-wm/issues/7) | Confirmed input bug. Keyboard/modifier MappingNotify rebuilds passive grabs using a fresh keysym map. |
| [#8](https://github.com/MidnightBSD/midnight-wm/issues/8) | Confirmed stale cache. Reload releases Cairo icon surfaces and clears lookup and dependent caches, then rereads the selected theme. The theme setting is in GTK settings, not the WM INI file as the report states. |

## Positive checks

- Desktop entry Exec and subprocess arguments use GIO or explicit argv; no
  generated `system()` or `popen()` shell command path was found.
- SQL object values use bound parameters.
- Notification raw-image dimensions, stride, channels, and payload length are
  checked before decoding pixels. Popup and action counts are bounded.
- D-Bus menu parsing limits depth, row count, and label length.
- Idle inhibitor cookies are tied to the requesting D-Bus connection and are
  removed when it disconnects; explicit locking remains independent.
- Settings writes preserve malformed files and use consistent replacement.
  Recent-file writes request private permissions. Thumbnail temporary names
  use mkstemp. SSH fallback agents use a private temporary directory.
- A source scan found no recognizable embedded private keys or common GitHub
  and AWS token formats. Pattern scanning is not proof that no secret exists.

## Remaining risks and recommended work

- **Filesystem race hardening:** launcher trust checks and reads are separate
  pathname operations. Recursive copies likewise use pathname checks and later
  opens. Stronger protection in directories writable by another user requires
  descriptor-based validation and traversal (`openat`/`fstat`/`O_NOFOLLOW`),
  including validated directory ownership. The fixes above do not claim that
  these operations are race-free.
- **Image decoder isolation:** icons, thumbnails, and wallpaper are decoded
  inside the desktop process through GdkPixbuf and its loaders. A crafted image
  or decoder flaw can affect the WM. Thumbnail compressed-file limits do not
  bound every decoder's expanded memory or CPU use. A resource-limited helper
  process would reduce this exposure. Third-party decoders were not audited.
- **Session service resource limits:** not every service has global quotas or
  rate limits. FileManager1 can be asked to open many folders, tray registrations
  can accumulate, and repeated property signals can schedule asynchronous work.
  Notification text can also be expensive to parse and lay out. The session
  bus's message limits are not a complete defense against a hostile peer.
- **Clipboard receive timeouts:** incoming data has a size bound, but an owner
  that never answers or never finishes INCR can leave a file-paste request
  waiting. Outgoing expiry added here does not fix incoming deadlines.

No remote code execution or privilege escalation was demonstrated. The review
found local resource exhaustion and unsafe input/file handling issues worth
fixing. Further fuzzing, sanitizer runs, dependency auditing, and testing as a
separate unprivileged account would increase confidence.

## Validation

Passed on the MidnightBSD host with Clang 19:

- Release build and all 20 CTest cases, including malformed WM_CLASS,
  clipboard limits and expired-slot recovery, copy containment/link handling,
  writable launcher refusal, and FileManager1 competing-owner checks.
- cppcheck and git diff whitespace checks.
- Isolated EWMH smoke: all eight frame resize directions, focus recovery,
  keycode remapping, SIGPIPE survival, hostile geometry hints, and existing
  workspace/ping/synchronized-resize tests.
- Isolated settings smoke: right/middle/left folder-close clicks, rendered
  red-to-blue theme icon replacement, and existing live settings checks.
- Isolated notifications smoke: actions, expiry, replacement, image validation,
  and existing protocol behavior.

New security X11 tests run only when CMake finds both Python 3 and Xvfb.
These checks ran under the available root test account with private displays,
HOME/XDG directories and session buses. They do not substitute for testing
cross-user filesystem races. Test sessions emitted existing GNOME Keyring/GVfs
warnings; the regression checks passed.
