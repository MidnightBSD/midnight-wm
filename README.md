# Midnight Workspace

Midnight Workspace is a Warp-inspired X11 desktop shell and window manager for
MidnightBSD. The current target is MidnightBSD 4.2 with Clang 19 and C++23.

## Build

The build needs CMake, pkg-config, XCB (including icccm, keysyms, XFixes, and
RandR, Xinerama), Cairo, Pango, GLib/GIO, GdkPixbuf, Garcon, and SQLite 3.
The companion `midnightbsd-settings` application also needs GTK 3.
The separate `midnightbsd-system-monitor` application uses GTK 3, libutil,
and libgeom.
The `midnightbsd-session` launcher and XSMP tests also need libSM and libICE.
Install the librsvg GdkPixbuf loader for SVG wallpapers; the mport includes
this runtime dependency. PNG, JPEG, and GIF loaders must also be available.

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

## Window shadows and compositing

Install [Picom](https://picom.app/) to enable soft window shadows. Midnight
Workspace starts Picom in the foreground with its XRender backend and a fixed
shadow preset, excluding the desktop, panel, and fullscreen windows. It does
not read or overwrite your separate Picom configuration. A compositor already
owning the screen is preserved; only a Picom process started by Midnight
Workspace is stopped when disabling shadows or ending the session.

Uncheck **Window shadows** in Settings → **Appearance**, or set
`compositing=false` in `[Workspace]`, to disable the session-owned compositor.
Changes apply live. This switch leaves an independently started compositor
running. Missing Picom or unsupported X11 extensions leave the desktop usable
without compositing. If Picom exits unexpectedly, toggle the setting off and
on to retry; the session does not repeatedly restart a failing compositor.

Run `python3 tests/compositor_smoke.py` with Picom installed to check actual
window and folder shadows, panel exclusion, live Settings, and shutdown in an
isolated Xvfb session. `--picom /path/to/picom` selects a test build. CTest also
checks selection ownership, missing-executable fallback, and process cleanup
without requiring Picom to be installed.

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
Workspace provides notification popups with application action buttons, images,
and urgency handling. Critical notifications remain until dismissed; resident
notifications can remain after an action. An existing notification daemon keeps
control.
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
Click the single speaker icon in the top panel to open the audio popup directly
below it; click again, click outside, or press Escape to close it. Drag the volume
slider and use **Mute** below it to mute or restore the previous stereo levels.
The mouse wheel changes volume in 5% steps. With the popup open, Left/Right
adjust volume and Space or M toggles mute. The control uses MidnightBSD's default
OSS `/dev/mixer`, falling back to PCM when master volume is unavailable.
The popup reports when no usable mixer is available.
Running programs show their window icon beside the task title, including in
the overflow menu. If a program supplies no usable icon, its installed desktop
entry is matched by window class, with a generic application icon as fallback.
Built-in folders use folder and Trash icons. Window icon changes apply live.

On systems with an ACPI battery, a battery icon and remaining percentage appear
next to the speaker button. Charging displays a lightning marker; low battery
uses a red fill. Click the indicator for power status and the remaining time
estimate when available. Status refreshes every 30 seconds. The indicator
is hidden when no battery is present; unavailable capacity is shown as `?`.
To hide it, uncheck **Show battery indicator** under Settings → **Desktop and panel**,
or set `show_battery=false` in `[Workspace]`. The change applies immediately.
Battery status uses MidnightBSD's `hw.acpi.battery.*` and `hw.acpi.acline`
sysctls, as the base system `batt` utility does.

Running MPRIS media players appear below the volume controls, with track and
artist information and previous, play/pause, and next controls. Click the player
name to cycle between multiple players. Unsupported actions are disabled, and
player discovery and commands use asynchronous D-Bus calls.

Right-click an application in **Programs** to see the additional actions from its
`.desktop` entry, such as a new window. Trusted executable desktop launchers also
expose their actions in the desktop context menu; their ownership and executable
permission are checked again before launching.

**Workspaces** in the Midnight menu switches between four desktops. Ctrl+Alt+Left
and Ctrl+Alt+Right also switch workspaces. **Show desktop** or Super+D hides
application and folder windows temporarily; activating a task restores them.
Applications can use standard EWMH workspace and Show Desktop requests. Clients
supporting `_NET_WM_PING` are marked unresponsive after a timeout, and clients
supporting `_NET_WM_SYNC_REQUEST` can acknowledge interactive resize updates.
Resize timeouts allow interaction to continue when an application stops responding.

Settings → **Session** offers optional built-in folder sessions. Enable
**Save folders on logout** to save open folder paths, normal geometry, workspace,
and maximized/shaded state when confirming **Exit X11 session…** or receiving a
normal termination signal. Cancelling logout leaves the running session intact.
Enable **Restore folders at login** to reopen the saved folders and select the
saved workspace; disable restoration to start fresh independently of saving.
Missing folders are skipped, and geometry is adjusted to the current monitors
and UI scale so restored windows remain reachable. Double-click a built-in
folder's title to maximize/restore it; middle-click to shade/expand it. Activating
its task expands a shaded folder. The title-bar minimize button hides a folder
while retaining its taskbar entry; click that task to restore it. Minimized
folders remain hidden across workspace and Show Desktop changes.

The Session page also offers **Restore applications at login** and **Save
application checkpoints**, independently of the folder options. Click **Apply**
to save these preferences; application saving uses the new preference at its next
checkpoint or logout, while restoration changes take effect at the next login.
Disable both restoration options to start fresh without deleting either saved
session. **Clear saved application session** removes only the application
checkpoint immediately, leaving running applications and the folder snapshot
intact. Enabled saving can create a new checkpoint later. Application restoration
requires the session companion and participating applications; applications
control which documents and tabs they restore.

The equivalent configuration is `[Session]` with `restore_folders=false` and
`save_on_logout=false` by default. Settings apply saving changes live; restoration
is checked at startup. **Clear saved folder session** removes the snapshot without
closing open folders. Saving on logout can create another snapshot afterward.
State is stored atomically in `$XDG_STATE_HOME/midnight-wm/session.ini` (normally
`~/.local/state/midnight-wm/session.ini`), with a private directory and file.
Malformed, incomplete, oversized, or unsupported-version snapshots are ignored
and preserved until cleared, including on logout. Write failures before atomic
replacement retain the existing snapshot; save errors go to the session log.

Participating XSMP applications can now be reopened when application restoration
is enabled. Applications control their own document and tab restoration; terminal
commands and other process state are not resumed. Participating windows restore
their saved placement and supported states. Application save prompts participate
in coordinated logout through the session companion. No restart
commands, credentials, or bus addresses are stored in the folder snapshot.

The Xsession entry now starts `midnightbsd-session`, which establishes the session
bus and then starts an XSMP manager before launching `midnight-wm`. For a plain
`startx`/`.xinitrc` session on MidnightBSD with ConsoleKit installed, use
`exec ck-launch-session midnightbsd-session` so ConsoleKit registers the session
before the window manager and its autostart applications begin. A display manager
that already registers a ConsoleKit session should launch `midnightbsd-session`
directly. Running `midnight-wm` directly continues to work without starting a
session manager, but a plain `.xinitrc` must still arrange a ConsoleKit session
for graphical Polkit authentication. A nonempty inherited `SESSION_MANAGER`
is preserved, along with its authority configuration, so an existing manager is
never replaced. The companion chooses its sibling `midnight-wm` executable when
available, then the configured installation path. `midnightbsd-session -- program
argument ...` can wrap another explicit argument list without shell expansion.

The companion accepts local Unix-domain ICE connections only and uses fresh
MIT-MAGIC-COOKIE-1 authentication for ICE and XSMP, with host-based authentication
disabled. Its separate private authority file lives in a temporary 0700 directory
under the user's private XDG runtime directory (or `/tmp` when unavailable) and
is removed when the companion exits. It does not modify the user's `.ICEauthority`.
It establishes `SESSION_MANAGER` and `ICEAUTHORITY` before the WM initializes the
keyring and launches XDG autostart applications. An existing reachable D-Bus bus
is retained; `dbus-run-session` owns a new bus when one is needed.

Registered clients receive an initial noninteractive checkpoint. Local/global
client checkpoint requests and `SIGUSR1` to the companion also request saving;
XSMP phase-2 requests are supported. Each checkpoint has a three-second deadline,
connections and properties have resource limits, and partial peer reads/writes
are bounded in the separate companion process. Applications can checkpoint
their own state and supply the restart arguments used on the next login.

Set `[Session] save_application_checkpoints=true` to persist successful global
checkpoints, including requests through `SIGUSR1`, to
`$XDG_STATE_HOME/midnight-wm/applications.ini` (normally
`~/.local/state/midnight-wm/applications.ini`). This preference defaults to false,
is read for each checkpoint, and is independent of folder saving. Initial and
local checkpoints remain memory-only. Confirmed logout takes a final global
checkpoint before client shutdown. A normal WM exit outside that flow requests a
noninteractive final checkpoint; crashes and termination signals do not save on exit.
A request during another save is ignored rather than merging save transactions.

The versioned application snapshot uses a private 0700 directory, a 0600 file,
and atomic replacement. Failed or timed-out client saves preserve the previous
checkpoint. A timed-out client must reconnect before it can participate in a
persisted checkpoint, so a late reply cannot be mistaken for a new save.
Malformed, unsupported, or insecure existing files are preserved
until explicitly removed. Restart argument bytes and boundaries are retained,
including non-UTF-8 filenames, without shell interpretation. Only client IDs,
programs, restart argument lists, working directories, and restart styles are
stored; environment and discard/shutdown commands are excluded. Restart arguments
can contain private document paths, so treat this file as private session data.
`RestartNever` is omitted; disconnected `RestartAnyway` clients retain their last
completed metadata. Other styles require a connected client at checkpoint time;
`RestartImmediately` does not yet trigger an immediate relaunch.
Set `[Session] restore_applications=true` to reopen the last valid application
checkpoint through `midnightbsd-session`. This defaults to false and takes effect
at the next login, independently of checkpoint saving and folder restoration.
The companion copies a validated checkpoint into its private runtime directory
for this login, then the WM launches its applications after establishing the new session
bus and keyring services. A running external session manager keeps ownership;
Midnight Workspace does not restore its own application snapshot under it.
The handoff variables are cleared before starting session services or applications.

Saved clients can reclaim their saved XSMP ID once during the first two minutes
of the new companion's lifetime. Unknown, duplicate and expired IDs register as
fresh clients. Checkpoint persistence waits until all expected IDs are claimed
or that window expires, preserving the source snapshot during startup. Saved
commands execute as argument lists with their saved working directories, current
session environment and closed internal descriptors. Missing executables, invalid
working directories and invalid executable formats are reported without stopping
the desktop; executable text without an interpreter is never retried as a shell
script. `RestartImmediately` participates in login restore but does not yet cause
an immediate restart after a client exits.

A single successful restore suppresses an eligible XDG autostart entry only when
both resolve to one unique direct executable. Failed restores leave autostart
eligible. Multiple profiles, multiple restores of the same executable and wrappers
are left to normal autostart rather than guessing which application they represent.
This conservative matching cannot eliminate duplicates in ambiguous cases.
Version 3 application checkpoints retain the version 2 window placement: normal geometry,
workspace, maximized axes, fullscreen and shaded state, keyed by XSMP client ID
and window role. Version 1 snapshots remain readable without placements; version 2 snapshots retain
their placements. Older builds reject version 3 snapshots rather than loading them. The WM
reads `SM_CLIENT_ID` from the window or its `WM_CLIENT_LEADER` and reads
`WM_WINDOW_ROLE` from the window itself. Missing or malformed identities and
ambiguous roles are omitted; a role-less window must be unique for its client.
Only windows belonging to successfully checkpointed clients are persisted.
Up to 512 placements fit within the existing one-MiB snapshot limit.

The companion requests capture through a private nonblocking inherited socket;
the WM replies through a private bounded runtime report, with no XCB dependency
in the companion. A 750-ms capture timeout preserves the previous checkpoint,
and late replies cannot authorize another request. Normal WM shutdown supplies
a final capture before dismantling its windows. Runtime files and descriptors
are removed on exit and are not inherited by restored or autostart applications.

New restored windows match by client ID and role once. Identity properties may
arrive during the first two seconds after management, within the companion's
two-minute restore window. Genuine input on the window cancels pending placement.
Normal geometry is recovered into the nearest current work area, excluding panel
and frame decorations; maximized/fullscreen state then uses the current monitor.
Shading preserves normal client dimensions. Sticky placement and minimized state
are not restored, and windows without XSMP identity are not matched by title or
class.

Settings → Session also offers **Also reopen identified apps without session
support**, off by default (`restore_desktop_applications=false`). With application
checkpoint saving enabled, this option records installed desktop-entry IDs for
managed non-XSMP windows advertising a valid `_GTK_APPLICATION_ID`. At the next
login, both this option and **Restore applications at login** must be enabled to
reopen them. It starts each recognized application once using its current desktop
entry through GIO, without copying `WM_COMMAND`, process arguments, working
directories, document paths, window placement, or process state. Multiple windows
of the same application collapse into one launch. These apps do not participate
in the companion's XSMP save prompts.

The fallback is deliberately limited to visible installed applications with a
valid direct executable. Hidden/missing entries, malformed IDs, and windows with
an XSMP identity (including a leader identity) are excluded. Relaunch revalidates
the registry entry, skips executable identities already represented by XSMP, and
omits ambiguous desktop entries sharing one executable. Successful relaunches
suppress only a unique autostart entry with the same complete installed command,
so wrappers and different profiles sharing an executable remain independent. X11 application
IDs are hints, not authenticated process identities; apps lacking a recognized
ID are omitted instead of guessing from titles or classes. Registry changes can
change which installed entry an ID resolves to. Version 3 snapshots store at most
128 unique desktop-entry IDs within the existing one-MiB private snapshot limit;
**Clear saved application session** clears both kinds of application data.

Confirming **Exit X11 session…** with `midnightbsd-session` starts coordinated
logout while the desktop continues dispatching input and drawing. Participating
applications receive an interactive XSMP save request, with one application
allowed to show its save prompt at a time. Cancelling an application's prompt,
a failed save, a disconnected client, or a save timeout cancels logout without
sending `Die`; the previous application checkpoint is preserved. The panel shows
a notice when logout is waiting or cancelled. Outstanding cancelled saves must
settle before another attempt; a timed-out client must reconnect so late replies
cannot authorize a later shutdown.

Ordinary application saves have a three-second deadline. Prompt requests queue
within a sixty-second overall logout deadline; exceeding it cancels logout.
After every participating client saves successfully, the companion captures
window placement and, when application checkpoint saving is enabled, writes the
final snapshot before sending `Die`. Capture or snapshot-write failure cancels
logout. It waits up to three seconds for clients to disconnect, then lets the WM
save its built-in folders and end the desktop. The companion does not signal
clients ignoring `Die`; see the session process cleanup below. Disabling application checkpoint saving still allows save
prompts and orderly logout, without replacing the saved application session.

On MidnightBSD the WM is the reaper for the processes it starts: applications
launched from the desktop, XDG autostart entries, and anything they leave running
in the background. Exited orphans are reaped while the desktop runs. When the WM
exits, it sends `SIGTERM` to every remaining descendant, waits up to three
seconds, and then sends `SIGKILL`, so nothing keeps a stale display, session bus,
or runtime directory after logout. Set `MIDNIGHT_KEEP_SESSION_PROCESSES=1` in
the session environment (for example in `.xinitrc`) to let tmux, screen, and
`nohup` jobs survive logout instead.

Only the WM's confirmed exit action initiates this flow; application requests to
end the desktop remain ignored. Standalone WMs and sessions under an existing
external manager retain their previous exit behavior. Crashes and termination
signals do not initiate interactive shutdown. The companion watches the WM it
launched, propagates that child's exit status, and removes
its owned sockets and authority file. A termination signal is forwarded only to
its owned child, with a bounded wait; registered applications are not signalled.

The separate XSMP metadata adapter and display-independent application/window
model follow the
[X.Org SMlib property types](https://xorg.freedesktop.org/releases/X11R7.7/doc/libSM/SMlib.html),
with the adapter preserving restart argument boundaries, bounding properties and
argument data, handling property deletion and restart-style hints, and excluding environment
and discard/shutdown commands from restart metadata. The manager separately
retains bounded protocol properties in memory for `GetProperties`, including
unknown properties; these are not written to a checkpoint file. Placement matching
requires an unambiguous session client ID and window role.
`xsmp_properties_test` exercises the adapter and window matching in Release
builds, including malformed properties and transactional rejection.
`python3 tests/xsmp_smoke.py` exercises real libSM clients, authentication failure,
initial/local/global checkpoints, phase-2 saves, unknown properties, deletion,
malformed updates, unresponsive clients, partial pre-auth frames, disconnections,
inherited managers, bus bootstrapping, argument boundaries, lifecycle cleanup,
and opt-in checkpoint persistence with failure/timeout preservation.
`coordinated_logout_smoke` checks interactive cancellation, save failures/timeouts,
serialized prompts, responsive workspace switching, checkpoint preservation/write
failures, successful phase-2 shutdown, and bounded client exit.
`application_checkpoint_test` covers snapshot validation, exact argument round
trips, privacy, malformed-state preservation and interrupted writes. The transport
test is registered with CTest when Python 3 and `dbus-run-session` are available.
Run `python3 tests/session_smoke.py --binary build/midnightbsd-session` to check
the real WM lifecycle through the companion, including an XSMP autostart client
that verifies the session bus and inherited agent before registering.
`python3 tests/application_restore_smoke.py` tests independent logins with a real
XSMP client, saved-ID adoption, autostart deduplication, new bus/agent inheritance,
literal arguments, disabled restore, corrupt-state preservation and cleanup.
Add `--placements` for window geometry/workspace/state checks, duplicate and
malformed roles, stalled-WM preservation, final shutdown capture and offscreen
recovery. Add `--matrix` to repeat those checks at all five supported resolutions.
Add `--changed-resolution-matrix` to save on a real 3840×2160 Xvfb screen,
restart the isolated X server, and restore onto each of the five supported sizes.
This checks recovery into the new work area and scale, including normal geometry
behind shading, maximization and fullscreen. The default CTest changed-resolution
case restores 4K sessions at 1280×720. These are actual screen sizes across logins,
not synthetic root resize events; they do not prove live RandR hotplug or offset
multi-monitor behavior. See [session validation](docs/session-validation.md).
`python3 tests/session_randr_smoke.py --module-path /path/to/private/modules`
uses a private Xorg dummy 0.4.x server for genuine RandR primary-output changes,
offsets, gaps, output disable/re-enable, and restoration with a missing output.
See the session validation guide for driver setup and optional CTest registration.
`application_restore_test` checks exact argument bytes, working directories,
spawn failures, descriptor/signal isolation and rejection of shell fallback.

Run `python3 tests/session_smoke.py --matrix` for isolated save/restore cycles at
all five supported resolutions, including cancelled logout, window states,
missing folders, offscreen geometry recovery, fresh startup, corrupt state, and
clearing through Settings. `session_test` covers parsing, privacy, atomic
replacement, interrupted writes, and monitor recovery without X11.

Middle-click an application's title bar to shade it to the title bar, and
middle-click again to expand it. The application remains mapped at its original
size; shading does not minimize it. Task activation, Alt+Tab selection, maximize,
and fullscreen expand shaded windows. EWMH clients can also request
`_NET_WM_STATE_SHADED`; allowed actions reflect whether shading and resizing are
currently available.

An external pager can set `_NET_DESKTOP_LAYOUT` to describe its workspace grid.
Ctrl+Alt+Left/Right/Up/Down follows that grid's physical directions, including
orientation and starting corner, wrapping within rows or columns and skipping
empty cells. Layout updates apply immediately. Missing or malformed layouts
use the default four horizontal workspaces; the WM does not overwrite the
pager's layout property.

EWMH activation requests use interaction timestamps to reject stale background
focus requests. `_NET_WM_USER_TIME=0` prevents initial focus; user-time helper
windows are supported. Clients without an initial user-time hint retain legacy
mapping behavior. Explicit taskbar/pager activation still restores a window and
switches to its workspace. This is desktop focus policy, not isolation between
X11 clients.

Applications requesting attention through EWMH or the ICCCM urgency hint get an
amber task outline and `!` marker without being focused. Overflow tasks also
carry the marker. Activating the window clears attention and urgency. The WM
publishes the read-only focused state and each window's allowed actions.
`ABOVE` and `BELOW` requests are mutually exclusive and preserve stacking layers
when focus changes; modal transients remain above their parent. Focused
fullscreen families stay above ordinary windows and docks. `SKIP_PAGER` is
supported independently of `SKIP_TASKBAR`.

Pagers can restack managed windows with `_NET_RESTACK_WINDOW` without changing
focus; desktop layers and transient ordering still apply. Applications can
request fullscreen across selected monitors with `_NET_WM_FULLSCREEN_MONITORS`.
Its four indices follow Xinerama ordering (top, bottom, left, right); servers
without active Xinerama expose the root screen as index zero. Requests apply
on fullscreen entry or immediately while fullscreen. Invalid indices or empty
rectangles are ignored. Monitor configuration changes clear the selection and
return windows to ordinary single-monitor fullscreen; applications can then
request a new selection.

Optional portal integration uses `deskutils/xdg-desktop-portal` and
`x11/xdg-desktop-portal-gtk`. The installed MidnightWorkspace portal configuration
selects GTK for file/application choosers and Midnight's Settings backend for
live dark-mode and accent-color preferences. User portal configuration takes
precedence. This adds chooser and appearance integration; screen sharing and
remote desktop require additional backend work.

When no other service owns `org.freedesktop.ScreenSaver`, Midnight handles
`Inhibit`/`UnInhibit` requests to suppress idle blanking and coordinates with
xscreensaver. Inhibitors are removed when their application disconnects. Explicit
screen locking remains available while inhibited.

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
cut, and paste. **View** switches between the icon grid and a list with Name,
Size, Type, and Modified columns. Ctrl+1 selects the grid; Ctrl+2 selects the list.
List columns hide progressively in narrow windows to keep filenames readable.
Use **View** to sort A–Z, Z–A, last modified (newest first), first modified
(oldest first), size (largest first), or type. Folders stay first in each order;
unknown sizes or modification times sort after known values. Size sorting uses
logical file lengths, without recursively measuring each subfolder. Click list
column headers to sort; Name and Modified toggle direction. Selection follows
the same item across sorting, refresh, and view changes. Arrow keys navigate
items, Enter opens the selection, and scrolling works in either view. Each new
folder starts in grid view with A–Z sorting; **View → Refresh** updates its
cached metadata and contents.
The bottom status bar keeps the object count on the left and the contents' size
on the right. With an item selected it shows only that item's size; selecting a
folder measures that folder's contents. Click empty space to return to the total.
Sizes use `humanize_number` with binary IEC units (KiB, MiB, GiB), and represent
logical file lengths, including nested contents, rather than allocated blocks.
Symbolic links contribute their own length and are not followed; special files
contribute no bytes. Calculation runs in the background, showing **Calculating…**
while pending. Unreadable entries or excessive nesting produce a **(partial)**
label; an inaccessible target shows **Size unavailable**. Directory changes and
View → Refresh recalculate the size, and completed results are resampled every
30 seconds to pick up changes inside nested folders.
On a session D-Bus, Midnight also offers `org.freedesktop.FileManager1`, so
applications can open folders, reveal selected files, and show file properties.
The `Midnight Workspace Files` application can be selected as the default
folder handler in Settings. Its `midnightbsd-open-folder` launcher sends local
paths or file URIs to the running workspace, opening built-in folder windows
without starting another window manager. The launcher also works if another
file manager owns the shared `org.freedesktop.FileManager1` name.

The `Home`, `Downloads`, `Documents`, `Pictures`, `Trash`, and `Templates`
shortcuts appear on the desktop by default. Downloads, Documents, Pictures, and
Templates use their configured XDG user directories, falling back to the
matching folder under the home directory when unset. Music, Videos, and Public
Share are available as optional shortcuts. An XDG directory set to the home
directory is hidden, as that setting disables the special directory. Physical
files appear from the user's XDG Desktop directory. Shell object
Icons fill downward from the upper left, then continue in the next column.
Overflow scrolls by columns using the wheel or Page Up/Page Down/Home/End. Object
metadata is stored in `$XDG_DATA_HOME/midnight-wm/workspace.db` (normally under
`~/.local/share`). Files and folders remain in the filesystem. Deleting a file
uses the freedesktop Trash mechanism.
In an object's Properties dialog, double-click Name or Target to select its
text. Use **Choose...** beside Icon to browse previews from the selected icon
theme, search by name, and choose an icon. **Default** restores the built-in
folder or file icon. Existing image paths continue to display.

Drag files between folder windows, the desktop, and other XDND-aware file
managers. Drag desktop files or folders onto Trash to move them to recoverable
Trash, or onto a folder icon to move them into that folder. Hold Ctrl when
starting a drag to copy instead. A preview follows the pointer with the item
icon, name, and Move or Copy label. Built-in desktop shortcuts remain in place.
The Trash window offers **Empty Trash…** in its File and right-click menus;
confirming permanently deletes all items in Trash.
Clipboard and drag operations support file URIs; text selection uses
standard X11 selection targets, including incremental transfers for large text.
The workspace keeps a text or file clipboard snapshot when its original owner
exits. The context menu provides object properties, shadows, templates, and
Trash restore actions. Copying or moving files runs in the background.

Open **Settings…** from the Midnight menu, or run `midnightbsd-settings`, to
change wallpaper, colors, fonts, scaling, panel text, clock format, and desktop
shortcuts. **Apply** saves your preferences and the running window manager
updates automatically within about two seconds. Closing without applying discards pending edits.
The settings app preserves unrelated INI keys and reports save errors.
The **Application defaults** page selects installed handlers for web links,
HTML files (including web page editors), email links, calendar files, folders,
text, PNG/JPEG images, and PDFs. It also selects
the terminal opened by the panel button. These choices are saved in the user
XDG MIME association files and the desktop's `xdg-terminals.list` preference;
they do not require a change to `midnight-wm/config.ini`.

Run `midnightbsd-system-monitor`, choose **System Monitor** from the Midnight
launcher below Mport Manager, or open it from Programs.
Its four tabs list processes; graph CPU, memory, and network activity; show
mounted file systems with capacity and available space; and summarize the CPU,
physical memory, swap allocation, OS release, and physical drives. Process and
resource data refresh every two seconds; mounts refresh every ten seconds.
The Hardware tab has a Refresh button for changes to swap or attached drives.

To edit preferences by hand, copy `share/midnight-wm/config.ini` to
`$XDG_CONFIG_HOME/midnight-wm/config.ini` (normally
`~/.config/midnight-wm/config.ini`) to change the panel label, clock format,
workspace appearance, or desktop shortcuts. In its `[Workspace]` section,
`dark_mode` defaults to `false` and selects the dark shell preset when set to
`true`. It applies to the shell, window frames, notifications, and D-Bus menus.
`ui_font` and `title_font` accept Pango family/style
descriptions, optionally followed by a point size from 6 to 32. Omitting the
size keeps each UI element's default size. The dark preset does not change
controls drawn by application toolkits. `desktop_color` remains a `#RRGGBB`
desktop background in either mode; `panel_color` optionally overrides the
panel color, and `accent_color` sets the accent color. The shared `ui_scale`
override is also configured here. By default the desktop shows
`mnbsd_bg_purple.png` from the `midnightbsd-backgrounds` package over the
lavender `desktop_color`; if that image is not installed, only the color is
shown. Set `desktop_image` to an absolute path,
`~/...` path, or path relative to this config file to use another wallpaper.
If a relative path is not found there, lookup searches the user XDG data
`backgrounds` directory, then system XDG data `backgrounds/midnightbsd` and
`backgrounds` directories, and the installation prefix
`share/backgrounds/midnightbsd`. The optional `midnightbsd-backgrounds` port
installs into `/usr/local/share/backgrounds/midnightbsd`, so a filename such
as `desktop_image=mnbsd_bg_purple.png` works directly. PNG,
JPEG, GIF, and SVG images are supported through GdkPixbuf; GIF uses its first
frame. The wallpaper repeats independently on each monitor. `desktop_image_mode`
can be `fill` (the default, centered crop) or `fit` (the whole image centered
with `desktop_color` showing around it). `desktop_color` also appears if the
wallpaper is unset or unreadable and behind transparent parts of an image.
Set `desktop_image=` (empty) to show only `desktop_color`. Saved changes are
reloaded automatically; malformed INI files leave the current settings active.
In the `[Desktop]` section,
`show_documents` and `show_pictures` default to `true`; `show_music`,
`show_videos`, and `show_public_share` default to `false`. Set any of these to
`true` or `false` to show or hide that shortcut. Changes apply automatically after
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

The panel network button sits beside the battery indicator and shows Wi-Fi,
Ethernet (including unknown link types), or a disconnected cable. Basic status
refreshes every 30 seconds without connectivity probes. Click it for active
interfaces, IP addresses, observed connection duration, and total received and
transmitted bytes. Wi-Fi also shows SSID, negotiated transmit link speed, and
802.11 standard when the driver exposes them. Details refresh every two seconds
only while the popup is open; close it with Escape or an outside click. Scroll
with the mouse wheel or arrow/Page keys for long interface lists. Connection
durations begin when the workspace first observes a link; counters are the
interface's totals, not traffic measured since opening the popup. Active lagg
members determine whether an aggregated connection is shown as wired or Wi-Fi.
When independent wired and wireless links are both active, the panel prefers
the wired icon; the popup still lists both.

## Design

The shell object model, file operations, and presentation code are separate
from X11 window management and selection handling. This keeps the shell
available for a later Wayland compositor backend. Wayland will require a new
compositor implementation; XCB window IDs and events do not cross that boundary.

The experimental MidnightBSD mport is in
`packaging/mports/x11-wm/midnight-wm/` and tracks the published 0.1.4 source
archive. Unreleased source changes may require additional installed files or
dependencies reflected in the bundled port. Generate source distributions with
`cpack --config build/CPackSourceConfig.cmake -G TGZ`.

The magnifying glass beside the panel clock opens Search Files. Enter a file
name or path and press Enter or Find to show the first 20 case-insensitive
literal substring matches from MidnightBSD's locate database, in database
order. Click a result to open it with its default application. Close dismisses
the window. Results reflect the system database's last update, so recently
created files may be missing and deleted files may still appear. An unavailable
database or failed file launch is reported in the search window.
