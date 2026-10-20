#pragma once

#include <chrono>

namespace midnight {

// Keeps processes started from the desktop inside the session. GLib spawns
// applications through an intermediate child, so launched applications and
// anything they daemonize are reparented here instead of to init. Exited
// orphans are reaped from tick(); children forked directly by this process are
// left to whoever started them (GLib child watches, GSubprocess, g_spawn_sync).
// stop() sends SIGTERM to every remaining descendant, waits a bounded grace
// period, and then sends SIGKILL, so nothing outlives the window manager with a
// stale DISPLAY, session bus, or runtime directory.
//
// Setting MIDNIGHT_KEEP_SESSION_PROCESSES=1 disables this, which lets tmux,
// screen, and nohup jobs survive logout. Without ConsoleKit or a display
// manager reaper, those processes then keep running after the X server exits.
//
// Construct after the session bus bootstrap: the dbus-run-session re-exec must
// not inherit reaper status.
class SessionReaper {
public:
  SessionReaper();
  ~SessionReaper();
  SessionReaper(const SessionReaper&) = delete;
  SessionReaper& operator=(const SessionReaper&) = delete;

  bool active() const { return active_; }
  // Reap exited orphans at most once per second.
  void tick();
  // Reap exited orphans now. Never waits for a child this process forked.
  void reap_orphans();
  // Terminate all remaining descendants and stop being a reaper.
  void stop();

private:
  bool active_{};
  std::chrono::steady_clock::time_point last_reap_{};
};

} // namespace midnight
