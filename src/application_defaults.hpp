// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace midnight {
enum class AppDefaultRole { browser, html, mail, calendar, terminal, files, text, images, pdf };
struct AppDefaultChoice {
  std::string id, name;
  bool operator==(const AppDefaultChoice&) const = default;
};
struct AppDefaultSetting { AppDefaultRole role; std::string id; };

const char* app_default_label(AppDefaultRole role);
std::vector<AppDefaultChoice> app_default_choices(AppDefaultRole role);
std::optional<AppDefaultChoice> current_app_default(AppDefaultRole role);
// Updates only selected roles in the current desktop's user XDG preferences.
// Reads existing files before writing, preserving unrelated entries and comments.
std::expected<void, std::string> save_app_defaults(const std::vector<AppDefaultSetting>& settings);
} // namespace midnight
