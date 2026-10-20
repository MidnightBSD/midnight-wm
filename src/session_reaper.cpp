#include "session_reaper.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
#include <sys/types.h>
#include <sys/procctl.h>
#include <sys/wait.h>
#include <csignal>
#include <unistd.h>
#endif

namespace midnight {

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
namespace {

constexpr auto grace_period = std::chrono::seconds(3);
constexpr auto poll_interval = std::chrono::milliseconds(100);

bool keep_session_processes() {
  const char* value = std::getenv("MIDNIGHT_KEEP_SESSION_PROCESSES");
  return value && std::string_view(value) == "1";
}

// At shutdown no GLib child watch will run again, so reap every exited child.
bool reap_all_and_check_empty() {
  while (waitpid(-1, nullptr, WNOHANG) > 0) {}
  procctl_reaper_status status{};
  if (procctl(P_PID, getpid(), PROC_REAP_STATUS, &status) != 0) return true;
  return status.rs_descendants == 0;
}

bool wait_for_descendants() {
  for (auto waited = std::chrono::milliseconds::zero(); waited < grace_period; waited += poll_interval) {
    if (reap_all_and_check_empty()) return true;
    std::this_thread::sleep_for(poll_interval);
  }
  return reap_all_and_check_empty();
}

void signal_descendants(int signal) {
  procctl_reaper_kill request{};
  request.rk_sig = signal;
  if (procctl(P_PID, getpid(), PROC_REAP_KILL, &request) != 0 && errno != ESRCH)
    std::cerr << "Session process signal: " << std::strerror(errno) << '\n';
}

} // namespace

SessionReaper::SessionReaper() {
  if (keep_session_processes()) return;
  if (procctl(P_PID, getpid(), PROC_REAP_ACQUIRE, nullptr) != 0) {
    std::cerr << "Session process reaper unavailable: " << std::strerror(errno) << '\n';
    return;
  }
  active_ = true;
}

void SessionReaper::reap_orphans() {
  if (!active_) return;
  procctl_reaper_status status{};
  if (procctl(P_PID, getpid(), PROC_REAP_STATUS, &status) != 0 || status.rs_children == 0) return;
  // Leave room for processes created between the two calls; later ones are
  // picked up on the next tick.
  std::vector<procctl_reaper_pidinfo> pids(status.rs_descendants + 16);
  procctl_reaper_pids request{};
  request.rp_count = static_cast<u_int>(pids.size());
  request.rp_pids = pids.data();
  if (procctl(P_PID, getpid(), PROC_REAP_GETPIDS, &request) != 0) return;
  for (const auto& info : pids) {
    if ((info.pi_flags & REAPER_PIDINFO_VALID) == 0) break;
    if ((info.pi_flags & REAPER_PIDINFO_CHILD) == 0 || (info.pi_flags & REAPER_PIDINFO_ZOMBIE) == 0) continue;
    // A child forked by this process starts its own subtree. Its owner waits
    // for it; reaping it here would steal that exit status.
    if (info.pi_subtree == info.pi_pid) continue;
    (void)waitpid(info.pi_pid, nullptr, WNOHANG);
  }
}

void SessionReaper::stop() {
  if (!active_) return;
  active_ = false;
  if (!reap_all_and_check_empty()) {
    signal_descendants(SIGTERM);
    if (!wait_for_descendants()) {
      signal_descendants(SIGKILL);
      if (!wait_for_descendants()) std::cerr << "Session processes still running after SIGKILL\n";
    }
  }
  if (procctl(P_PID, getpid(), PROC_REAP_RELEASE, nullptr) != 0)
    std::cerr << "Session process reaper release: " << std::strerror(errno) << '\n';
}
#else
SessionReaper::SessionReaper() = default;
void SessionReaper::reap_orphans() {}
void SessionReaper::stop() {}
#endif

SessionReaper::~SessionReaper() { stop(); }

void SessionReaper::tick() {
  if (!active_) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - last_reap_ < std::chrono::seconds(1)) return;
  last_reap_ = now;
  reap_orphans();
}

} // namespace midnight
