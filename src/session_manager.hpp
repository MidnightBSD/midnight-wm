// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <expected>
#include <memory>
#include <string>

namespace midnight {
// Owns XSMP/ICE, with no XCB connection. Runs in the companion process so a
// broken ICE peer cannot block the desktop's event dispatch or drawing.
class SessionManager {
public:
  SessionManager();
  ~SessionManager();
  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;
  std::expected<void, std::string> initialize();
  int release_window_child_fd();
  void tick(int timeout_ms = 100);
  void checkpoint(bool persist = true, bool final_checkpoint = false);
  bool checkpoint_pending() const;
  bool logout_completed() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace midnight
