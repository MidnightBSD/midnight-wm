// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "application_session.hpp"

#include <algorithm>
#include <filesystem>

namespace midnight {
std::expected<void, std::string> validate_application_restart(const ApplicationRestart& restart) {
  const auto text = [](const std::string& value, std::size_t limit) {
    return value.size() <= limit && value.find('\0') == std::string::npos;
  };
  if (restart.client_id.empty() || !text(restart.client_id, 256) ||
      std::ranges::any_of(restart.client_id, [](unsigned char ch) { return ch < 32 || ch == 127; }))
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

std::optional<std::size_t> match_application_window(std::span<const ApplicationWindow> windows,
    std::string_view client_id, std::string_view role, std::span<const std::size_t> claimed) {
  if (client_id.empty() || client_id.size() > 256 || role.size() > 4096 ||
      client_id.find('\0') != std::string_view::npos || role.find('\0') != std::string_view::npos)
    return std::nullopt;
  std::optional<std::size_t> result;
  for (std::size_t i = 0; i < windows.size(); ++i) {
    if (windows[i].client_id != client_id || windows[i].role != role ||
        std::ranges::find(claimed, i) != claimed.end()) continue;
    if (result) return std::nullopt;
    result = i;
  }
  return result;
}
} // namespace midnight
