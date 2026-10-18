// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "application_session.hpp"
#include <chrono>
#include <functional>

namespace midnight {
using WindowReport = std::expected<std::vector<ApplicationWindow>, std::string>;
// Native temporary format, bounded and private; persistent snapshots use INI.
std::expected<void, std::string> write_window_report(const std::filesystem::path& path, const WindowReport& windows);
WindowReport read_window_report(const std::filesystem::path& path);
class SessionWindowChannel {
public:
  SessionWindowChannel() = default;
  SessionWindowChannel(const SessionWindowChannel&) = delete;
  SessionWindowChannel& operator=(const SessionWindowChannel&) = delete;
  ~SessionWindowChannel();
  std::expected<void, std::string> initialize(const std::filesystem::path& directory);
  int release_child_fd();
  void tick();
  void request();
  bool attached() const { return ready_; }
  bool pending() const { return waiting_; }
  WindowReport result() const;
private:
  int parent_{-1}, child_{-1};
  std::filesystem::path path_;
  bool ready_{}, waiting_{}, fresh_{}, finished_{}, failed_{};
  std::chrono::steady_clock::time_point deadline_{};
};
// Consumes and clears inherited handoff variables before other children start.
class SessionWindowReporter {
public:
  SessionWindowReporter();
  SessionWindowReporter(const SessionWindowReporter&) = delete;
  SessionWindowReporter& operator=(const SessionWindowReporter&) = delete;
  ~SessionWindowReporter();
  void ready();
  void tick(const std::function<WindowReport()>& capture);
  void finish(const WindowReport& windows);
private:
  void publish(const WindowReport& windows, char success);
  int fd_{-1};
  std::filesystem::path path_;
};
} // namespace midnight
