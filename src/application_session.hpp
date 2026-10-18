// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "session.hpp"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace midnight {
enum class RestartStyle { if_running, anyway, immediately, never };

// No environment, bus address, shell expansion, or discard/shutdown commands.
// This is metadata, not authorization to execute a command.
struct ApplicationRestart {
  std::string client_id;
  std::string program;
  std::vector<std::string> arguments;
  std::string directory;
  RestartStyle style{RestartStyle::if_running};
  bool operator==(const ApplicationRestart&) const = default;
};

std::expected<void, std::string> validate_application_restart(const ApplicationRestart& restart);

struct ApplicationWindow {
  std::string client_id;
  std::string role;
  SessionGeometry geometry;
  std::uint32_t workspace{};
  bool maximized_horz{}, maximized_vert{}, fullscreen{}, shaded{};
};

// Ambiguous or already claimed windows never receive a saved placement.
// A missing role can match only a unique unclaimed role-less window.
std::optional<std::size_t> match_application_window(
    std::span<const ApplicationWindow> windows, std::string_view client_id,
    std::string_view role, std::span<const std::size_t> claimed = {});
} // namespace midnight
