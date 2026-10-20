// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "session_reaper.hpp"

#include <cstdlib>
#include <iostream>

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
#include <sys/types.h>
#include <sys/procctl.h>
#include <sys/wait.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <thread>
#include <unistd.h>

namespace {
bool check(bool result, const char* message) {
  if (!result) std::cerr << message << '\n';
  return result;
}

// Wait until pid is an exited child of this process without reaping it.
bool wait_exited(pid_t pid) {
  for (int i = 0; i < 500; ++i) {
    siginfo_t info{};
    if (waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// Start a grandchild through an intermediate child, as GLib does for
// asynchronous launches, so it is reparented to this reaper.
pid_t orphan(bool ignore_term) {
  int pipefd[2];
  if (pipe(pipefd) != 0) return -1;
  const pid_t intermediate = fork();
  if (intermediate == 0) {
    const pid_t grandchild = fork();
    if (grandchild == 0) {
      if (ignore_term) std::signal(SIGTERM, SIG_IGN);
      close(pipefd[0]);
      close(pipefd[1]);
      for (;;) pause();
    }
    (void)!write(pipefd[1], &grandchild, sizeof(grandchild));
    _exit(0);
  }
  close(pipefd[1]);
  pid_t grandchild = -1;
  if (intermediate > 0) {
    waitpid(intermediate, nullptr, 0);
    if (read(pipefd[0], &grandchild, sizeof(grandchild)) != sizeof(grandchild)) grandchild = -1;
  }
  close(pipefd[0]);
  return grandchild;
}

bool gone(pid_t pid) { return kill(pid, 0) != 0 && errno == ESRCH; }
} // namespace

int main() {
  bool ok = true;
  setenv("MIDNIGHT_KEEP_SESSION_PROCESSES", "1", 1);
  {
    midnight::SessionReaper reaper;
    ok &= check(!reaper.active(), "opt-out still acquired the reaper");
  }
  unsetenv("MIDNIGHT_KEEP_SESSION_PROCESSES");

  midnight::SessionReaper reaper;
  if (!check(reaper.active(), "reaper was not acquired")) return 1;

  // A direct child belongs to whoever forked it; its status must survive.
  const pid_t direct = fork();
  if (direct == 0) _exit(7);
  ok &= check(direct > 0 && wait_exited(direct), "direct child did not exit");
  reaper.reap_orphans();
  int status = 0;
  ok &= check(waitpid(direct, &status, WNOHANG) == direct && WIFEXITED(status) && WEXITSTATUS(status) == 7,
              "reaper consumed a direct child's exit status");

  // An exited orphan is reaped instead of remaining a zombie.
  const pid_t exited = orphan(false);
  ok &= check(exited > 0, "could not start orphan");
  kill(exited, SIGTERM);
  ok &= check(wait_exited(exited), "orphan was not reparented or did not exit");
  reaper.reap_orphans();
  ok &= check(waitpid(exited, nullptr, WNOHANG) == -1 && errno == ECHILD, "exited orphan was not reaped");

  // stop() terminates survivors, escalating to SIGKILL, and releases the reaper.
  const pid_t polite = orphan(false);
  const pid_t stubborn = orphan(true);
  ok &= check(polite > 0 && stubborn > 0, "could not start session orphans");
  reaper.stop();
  ok &= check(!reaper.active(), "reaper still active after stop");
  ok &= check(gone(polite), "SIGTERM did not stop an orphan");
  ok &= check(gone(stubborn), "SIGKILL did not stop an orphan ignoring SIGTERM");
  procctl_reaper_status reaper_status{};
  ok &= check(procctl(P_PID, getpid(), PROC_REAP_STATUS, &reaper_status) == 0 &&
                  (reaper_status.rs_flags & REAPER_STATUS_OWNED) == 0,
              "reaper status was not released");
  return ok ? 0 : 1;
}
#else
int main() {
  midnight::SessionReaper reaper;
  if (reaper.active()) {
    std::cerr << "reaper unexpectedly active on this platform\n";
    return 1;
  }
  return 0;
}
#endif
