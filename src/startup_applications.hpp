// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace midnight {

struct StartupApplication {
  std::string id;
  std::string name;
  std::string command;
  std::filesystem::path path;
  bool enabled{};
  bool available{};
  bool user_entry{};
};

// Show the effective XDG autostart file for each desktop-file ID, including
// disabled entries. The first configuration root has the highest priority.
std::vector<StartupApplication> startup_applications(
    const std::vector<std::filesystem::path>& config_roots);

std::expected<void, std::string> set_startup_application_enabled(
    const StartupApplication& application, bool enabled,
    const std::filesystem::path& user_config_root);
std::expected<std::string, std::string> add_startup_application(
    const std::string& name, const std::string& command,
    const std::filesystem::path& user_config_root);
std::expected<void, std::string> remove_startup_application(
    const StartupApplication& application, const std::filesystem::path& user_config_root);

} // namespace midnight
