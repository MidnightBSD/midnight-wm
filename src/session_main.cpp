// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "session_manager.hpp"
#include "session_bus.hpp"

#include <glib.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MIDNIGHT_WM_VERSION
#define MIDNIGHT_WM_VERSION "dev"
#endif
#ifndef MIDNIGHT_WM_PROGRAM
#define MIDNIGHT_WM_PROGRAM "/usr/local/bin/midnight-wm"
#endif

namespace {
volatile std::sig_atomic_t stop_signal{};
volatile std::sig_atomic_t checkpoint_requested{};
void stop(int signal) { stop_signal = signal; }
void request_checkpoint(int) { checkpoint_requested = 1; }

std::string wm_program(const char* program) {
  char* located = g_find_program_in_path(program);
  std::error_code error;
  const auto executable = std::filesystem::canonical(located ? located : program, error);
  g_free(located);
  if (!error) {
    const auto sibling = executable.parent_path() / "midnight-wm";
    if (access(sibling.c_str(), X_OK) == 0) return sibling.string();
  }
  return MIDNIGHT_WM_PROGRAM;
}
}

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << "Midnight Workspace session " << MIDNIGHT_WM_VERSION << '\n';
    return 0;
  }
  if (argc > 1 && (std::string_view(argv[1]) != "--" || argc < 3)) {
    std::cerr << "Usage: midnightbsd-session [--version | -- program [argument ...]]\n";
    return 2;
  }
  // Start the bus outside this process's managed child. The existing helper
  // keeps display-manager buses and its wrapper retains ownership of new buses.
  midnight::ensure_session_bus(argc, argv);
  std::string program = wm_program(argv[0]);
  std::vector<char*> command;
  if (argc > 2) for (int i = 2; i < argc; ++i) command.push_back(argv[i]);
  else command.push_back(program.data());
  command.push_back(nullptr);
  if (const char* inherited = std::getenv("SESSION_MANAGER"); inherited && *inherited) {
    g_unsetenv("MIDNIGHT_WM_WINDOW_FD");
    g_unsetenv("MIDNIGHT_WM_WINDOW_REPORT");
    g_unsetenv("MIDNIGHT_WM_RESTORE_PLAN");
    g_unsetenv("MIDNIGHT_WM_RESTORE_MANAGER");
    execvp(command[0], command.data());
    std::cerr << "Could not launch session: " << std::strerror(errno) << '\n';
    return 1;
  }

  // Handoff state belongs only to this new companion, never an inherited one.
  g_unsetenv("MIDNIGHT_WM_WINDOW_FD");
  g_unsetenv("MIDNIGHT_WM_WINDOW_REPORT");
  g_unsetenv("MIDNIGHT_WM_RESTORE_PLAN");
  g_unsetenv("MIDNIGHT_WM_RESTORE_MANAGER");
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  std::signal(SIGUSR1, request_checkpoint);
  midnight::SessionManager manager;
  if (auto initialized = manager.initialize(); !initialized) {
    std::cerr << "XSMP session unavailable: " << initialized.error() << '\n';
    return 1;
  }
  const int window_fd = manager.release_window_child_fd();
  const pid_t child = fork();
  if (child < 0) {
    close(window_fd);
    std::cerr << "Could not start window manager: " << std::strerror(errno) << '\n';
    return 1;
  }
  if (!child) {
    if (fcntl(window_fd, F_SETFD, 0) < 0) _exit(127);
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGUSR1, SIG_DFL);
    execvp(command[0], command.data());
    constexpr char message[] = "Could not execute session program\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(127);
  }
  close(window_fd);
  g_unsetenv("MIDNIGHT_WM_WINDOW_FD");
  g_unsetenv("MIDNIGHT_WM_WINDOW_REPORT");
  int status{};
  std::optional<std::chrono::steady_clock::time_point> stopping;
  for (;;) {
    const auto waited = waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) { status = 1 << 8; break; }
    if (stop_signal && !stopping) {
      (void)kill(child, stop_signal);
      stopping = std::chrono::steady_clock::now();
    }
    if (stopping && std::chrono::steady_clock::now() - *stopping > std::chrono::seconds(5))
      (void)kill(child, SIGKILL);
    if (checkpoint_requested) { checkpoint_requested = 0; manager.checkpoint(); }
    manager.tick();
  }
  // Coordinated logout already saved before asking clients to exit. Keep the
  // standalone/termination fallback without overwriting that completed snapshot.
  if (!manager.logout_completed() && !manager.final_checkpoint_complete())
    manager.checkpoint(!stop_signal && WIFEXITED(status) && WEXITSTATUS(status) == 0, true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (manager.checkpoint_pending() && std::chrono::steady_clock::now() < deadline) manager.tick();
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}
