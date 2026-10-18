// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "application_session.hpp"
#include <sys/types.h>

namespace midnight {
// Shared with autostart matching; canonical executable identity, never a shell.
std::optional<std::string> resolve_session_executable(const std::string& executable);
class ApplicationRestorer {
public:
  ~ApplicationRestorer();
  ApplicationRestorer() = default;
  ApplicationRestorer(const ApplicationRestorer&) = delete;
  ApplicationRestorer& operator=(const ApplicationRestorer&) = delete;
  // Caller supplies an explicitly authorized, validated plan after services are
  // ready. Spawn success is used for conservative autostart deduplication.
  std::vector<std::string> restore(const ApplicationCheckpoint& checkpoint);
  void tick();
  const std::vector<std::string>& launched_executables() const { return launched_; }
private:
  bool started_{};
  std::vector<pid_t> children_;
  std::vector<std::string> launched_;
};
// Consume only the owning companion's private, per-login plan. Clear handoff
// variables before any application or autostart process can inherit them.
std::expected<std::optional<ApplicationCheckpoint>, std::string> take_application_restore_plan(bool enabled);
} // namespace midnight
