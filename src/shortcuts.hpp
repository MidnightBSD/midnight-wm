// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <xcb/xcb.h>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace midnight {

enum class ShortcutAction : std::size_t {
  workspace_left, workspace_right, workspace_up, workspace_down,
  show_desktop, switch_window, close_window,
  move_window, resize_window, minimize_window, maximize_window, switch_window_backward, count
};

struct KeyChord {
  std::uint32_t keysym{};
  std::uint16_t modifiers{};
  bool operator==(const KeyChord&) const = default;
};

using Shortcuts = std::array<std::optional<KeyChord>, static_cast<std::size_t>(ShortcutAction::count)>;

[[nodiscard]] Shortcuts default_shortcuts();
[[nodiscard]] const char* shortcut_key(ShortcutAction action);
[[nodiscard]] const char* shortcut_label(ShortcutAction action);
[[nodiscard]] std::expected<std::optional<KeyChord>, std::string> parse_shortcut(std::string_view text);
[[nodiscard]] std::string format_shortcut(std::optional<KeyChord> chord);
[[nodiscard]] bool shortcuts_conflict(const Shortcuts& shortcuts);
[[nodiscard]] std::optional<ShortcutAction> shortcut_action(const Shortcuts& shortcuts,
    std::uint32_t keysym, std::uint16_t modifiers);

} // namespace midnight
