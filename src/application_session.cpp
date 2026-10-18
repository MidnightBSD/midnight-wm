// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "application_session.hpp"

#include <algorithm>
#include <filesystem>

namespace midnight {
namespace {
bool valid_client_id(std::string_view id) {
  return !id.empty() && id.size() <= 256 && std::ranges::none_of(id, [](unsigned char ch) {
    return ch < 32 || ch == 127;
  });
}
} // namespace
std::expected<void, std::string> validate_application_restart(const ApplicationRestart& restart) {
  const auto text = [](const std::string& value, std::size_t limit) {
    return value.size() <= limit && value.find('\0') == std::string::npos;
  };
  if (!valid_client_id(restart.client_id))
    return std::unexpected("Invalid session client ID");
  if (restart.program.empty() || !text(restart.program, 4096))
    return std::unexpected("Invalid application program");
  if (restart.arguments.empty() || restart.arguments.size() > 128 || restart.arguments.front().empty())
    return std::unexpected("Invalid restart argument count or executable");
  std::size_t bytes = 0;
  for (const auto& argument : restart.arguments) {
    if (!text(argument, 8192)) return std::unexpected("Invalid restart argument");
    bytes += argument.size() + 1;
    if (bytes > 65536) return std::unexpected("Restart arguments exceed the size limit");
  }
  const std::filesystem::path executable(restart.arguments.front());
  if (restart.arguments.front().front() == '-' ||
      (!executable.is_absolute() && executable.has_parent_path()))
    return std::unexpected("Restart executable must be absolute or a name in PATH");
  if (!text(restart.directory, 4096) ||
      (!restart.directory.empty() && !std::filesystem::path(restart.directory).is_absolute()))
    return std::unexpected("Invalid application working directory");
  switch (restart.style) {
    case RestartStyle::if_running: case RestartStyle::anyway:
    case RestartStyle::immediately: case RestartStyle::never: return {};
  }
  return std::unexpected("Invalid application restart style");
}

std::expected<void, std::string> validate_application_window(const ApplicationWindow& window) {
  if (!valid_client_id(window.client_id) || window.role.size() > 4096 ||
      window.role.find('\0') != std::string::npos)
    return std::unexpected("Invalid application window identity");
  const auto& geometry = window.geometry;
  if (window.workspace >= 4 || geometry.x < -32768 || geometry.x > 32767 ||
      geometry.y < -32768 || geometry.y > 32767 || geometry.width < 1 || geometry.width > 32767 ||
      geometry.height < 1 || geometry.height > 32767 || (window.fullscreen && window.shaded))
    return std::unexpected("Invalid application window placement");
  return {};
}

std::optional<std::size_t> match_application_window(std::span<const ApplicationWindow> windows,
    std::string_view client_id, std::string_view role, std::span<const std::size_t> claimed) {
  if (!valid_client_id(client_id) || role.size() > 4096 || role.find('\0') != std::string_view::npos)
    return std::nullopt;
  std::optional<std::size_t> result;
  for (std::size_t i = 0; i < windows.size(); ++i) {
    if (windows[i].client_id != client_id || windows[i].role != role) continue;
    if (result) return std::nullopt;
    result = i;
  }
  // Claiming one ambiguous candidate must not make the remaining candidate
  // appear unique. Check ambiguity against the complete saved set first.
  if (result && std::ranges::find(claimed, *result) != claimed.end()) return std::nullopt;
  return result;
}
} // namespace midnight
