// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "application_restore.hpp"
#include "config.hpp"
#include <glib.h>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

std::string encoded(const std::string& text) {
  char* value = g_base64_encode(reinterpret_cast<const guchar*>(text.data()), text.size());
  std::string result(value); g_free(value); return result;
}
int main(int argc, char** argv) {
  if (argc > 2 && std::string_view(argv[1]) == "--report") {
    const char* extra = g_getenv("MIDNIGHT_RESTORE_TEST_FD");
    const bool closed = extra && fcntl(std::atoi(extra), F_GETFD) < 0;
    struct sigaction action{}; sigaction(SIGPIPE, nullptr, &action);
    std::ofstream report(argv[2]);
    report << encoded(std::filesystem::current_path().string()) << '\n'
           << encoded(g_getenv("MIDNIGHT_RESTORE_TEST_VALUE")) << '\n'
           << (action.sa_handler == SIG_DFL) << '\n' << closed << '\n';
    for (int i = 0; i < argc; ++i) report << encoded(argv[i]) << '\n';
    return 0;
  }
  char temporary[] = "/tmp/midnight-restore-test-XXXXXX";
  if (!mkdtemp(temporary)) return 1;
  const std::filesystem::path root(temporary), report = root / "report";
  bool passed = true;
  const auto check = [&](bool value, const char* message) {
    if (!value) { passed = false; std::cerr << message << '\n'; }
  };
  const auto executable = std::filesystem::canonical(argv[0]).string();
  const int extra = open("/dev/null", O_RDONLY);
  g_setenv("MIDNIGHT_RESTORE_TEST_FD", std::to_string(extra).c_str(), true);
  g_setenv("MIDNIGHT_RESTORE_TEST_VALUE", "new session value", true);
  std::signal(SIGPIPE, SIG_IGN);
  midnight::ApplicationRestart good{"saved-id", "fixture", {executable, "--report", report.string(),
    "literal $(touch should-not-exist); with spaces", "", std::string("byte-\xff", 6)}, root.string()};
  auto missing = good; missing.client_id = "missing"; missing.arguments[0] = "/no/such/midnight-restore";
  auto directory = good; directory.client_id = "directory"; directory.directory = (root / "missing").string();
  const auto raw_path = root / "raw-executable";
  std::ofstream(raw_path) << "touch should-not-exist\n";
  chmod(raw_path.c_str(), 0700);
  auto raw = good; raw.client_id = "raw"; raw.arguments[0] = raw_path.string();
  auto never = good; never.client_id = "never"; never.style = midnight::RestartStyle::never;
  midnight::ApplicationRestorer restorer;
  const auto errors = restorer.restore({{good, missing, directory, raw, never}});
  check(errors.size() == 3 && restorer.launched_executables().size() == 1, "spawn failure accounting incorrect");
  std::string expected = encoded(root.string()) + "\n" + encoded("new session value") + "\n1\n1\n";
  for (const auto& argument : good.arguments) expected += encoded(argument) + "\n";
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  std::string received;
  do {
    std::ifstream input(report);
    received.assign(std::istreambuf_iterator<char>(input), {});
    if (received == expected) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  check(received == expected, "cwd, argv, new environment, signals or descriptor isolation changed");
  check(!std::filesystem::exists(root / "should-not-exist"), "raw executable was interpreted by shell");
  restorer.restore({{good}});
  check(restorer.launched_executables().size() == 1, "restore ran twice");
  close(extra);
  auto defaults = midnight::parse_config("");
  auto enabled = midnight::parse_config("[Session]\nrestore_applications=true\n");
  check(defaults && !defaults->restore_applications && enabled && enabled->restore_applications, "restore must be opt-in");
  const auto plan = root / "private/restore.ini";
  midnight::ApplicationSessionStore store(plan);
  check(bool(store.save({{good}})), "cannot create private plan");
  g_setenv("SESSION_MANAGER", "local/owned", true);
  g_setenv("MIDNIGHT_WM_RESTORE_MANAGER", "local/other", true);
  g_setenv("MIDNIGHT_WM_RESTORE_PLAN", plan.c_str(), true);
  auto taken = midnight::take_application_restore_plan(true);
  check(taken && !*taken && !g_getenv("MIDNIGHT_WM_RESTORE_PLAN"), "external manager handoff accepted or leaked");
  g_setenv("MIDNIGHT_WM_RESTORE_MANAGER", "local/owned", true);
  g_setenv("MIDNIGHT_WM_RESTORE_PLAN", plan.c_str(), true);
  taken = midnight::take_application_restore_plan(false);
  check(taken && !*taken && !g_getenv("MIDNIGHT_WM_RESTORE_MANAGER"), "disabled handoff accepted or leaked");
  g_setenv("MIDNIGHT_WM_RESTORE_MANAGER", "local/owned", true);
  g_setenv("MIDNIGHT_WM_RESTORE_PLAN", plan.c_str(), true);
  taken = midnight::take_application_restore_plan(true);
  check(taken && *taken && (**taken).applications == std::vector{good}, "private handoff failed");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  restorer.tick();
  std::filesystem::remove_all(root);
  return passed ? 0 : 1;
}
