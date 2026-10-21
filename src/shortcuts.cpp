// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include "shortcuts.hpp"

#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <sstream>

namespace midnight {
namespace {
constexpr std::array<const char*, static_cast<std::size_t>(ShortcutAction::count)> keys{
    "workspace_left", "workspace_right", "workspace_up", "workspace_down",
    "show_desktop", "switch_window", "close_window",
    "move_window", "resize_window", "minimize_window", "maximize_window", "switch_window_backward"};
constexpr std::array<const char*, keys.size()> labels{
    "Workspace left", "Workspace right", "Workspace up", "Workspace down",
    "Show desktop", "Switch window", "Close active window",
    "Move active window", "Resize active window", "Minimize active window",
    "Maximize/restore active window", "Switch window backward"};
constexpr std::uint16_t primary_modifiers = XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4;
constexpr std::uint16_t supported_modifiers = primary_modifiers | XCB_MOD_MASK_SHIFT;

std::string lowercase(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char ch) { return std::tolower(ch); });
  return result;
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
  return value;
}

std::optional<std::uint32_t> key_from_name(std::string_view name) {
  const auto lower = lowercase(name);
  if (lower.size() == 1) {
    if (lower[0] >= 'a' && lower[0] <= 'z') return XK_a + lower[0] - 'a';
    if (lower[0] >= '0' && lower[0] <= '9') return XK_0 + lower[0] - '0';
  }
  constexpr std::array<std::pair<std::string_view, std::uint32_t>, 25> named{{
      {"left", XK_Left}, {"right", XK_Right}, {"up", XK_Up}, {"down", XK_Down},
      {"tab", XK_Tab}, {"escape", XK_Escape}, {"return", XK_Return},
      {"space", XK_space}, {"backspace", XK_BackSpace}, {"delete", XK_Delete},
      {"insert", XK_Insert}, {"home", XK_Home}, {"end", XK_End},
      {"page_up", XK_Page_Up}, {"page_down", XK_Page_Down},
      {"comma", XK_comma}, {"period", XK_period}, {"slash", XK_slash},
      {"minus", XK_minus}, {"equal", XK_equal}, {"semicolon", XK_semicolon},
      {"bracketleft", XK_bracketleft}, {"bracketright", XK_bracketright},
      {"backslash", XK_backslash}, {"grave", XK_grave}}};
  for (const auto& [key, value] : named) if (lower == key) return value;
  if (lower.size() >= 2 && lower.front() == 'f') {
    unsigned number = 0;
    const auto result = std::from_chars(lower.data() + 1, lower.data() + lower.size(), number);
    if (result.ec == std::errc{} && result.ptr == lower.data() + lower.size() && number >= 1 && number <= 35)
      return XK_F1 + number - 1;
  }
  if (lower.starts_with("0x") && lower.size() > 2) {
    std::uint32_t value = 0;
    const auto result = std::from_chars(lower.data() + 2, lower.data() + lower.size(), value, 16);
    if (result.ec == std::errc{} && result.ptr == lower.data() + lower.size() && value != 0)
      return value;
  }
  return std::nullopt;
}

std::string name_for_key(std::uint32_t key) {
  if (key >= XK_a && key <= XK_z) return std::string(1, static_cast<char>('A' + key - XK_a));
  if (key >= XK_0 && key <= XK_9) return std::string(1, static_cast<char>('0' + key - XK_0));
  if (key >= XK_F1 && key <= XK_F35) return "F" + std::to_string(key - XK_F1 + 1);
  constexpr std::array<std::pair<std::uint32_t, const char*>, 25> named{{
      {XK_Left, "Left"}, {XK_Right, "Right"}, {XK_Up, "Up"}, {XK_Down, "Down"},
      {XK_Tab, "Tab"}, {XK_Escape, "Escape"}, {XK_Return, "Return"},
      {XK_space, "Space"}, {XK_BackSpace, "BackSpace"}, {XK_Delete, "Delete"},
      {XK_Insert, "Insert"}, {XK_Home, "Home"}, {XK_End, "End"},
      {XK_Page_Up, "Page_Up"}, {XK_Page_Down, "Page_Down"},
      {XK_comma, "Comma"}, {XK_period, "Period"}, {XK_slash, "Slash"},
      {XK_minus, "Minus"}, {XK_equal, "Equal"}, {XK_semicolon, "Semicolon"},
      {XK_bracketleft, "BracketLeft"}, {XK_bracketright, "BracketRight"},
      {XK_backslash, "Backslash"}, {XK_grave, "Grave"}}};
  for (const auto& [value, name] : named) if (key == value) return name;
  std::ostringstream result;
  result << "0x" << std::hex << key;
  return result.str();
}
} // namespace

Shortcuts default_shortcuts() {
  return {{KeyChord{XK_Left, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1},
           KeyChord{XK_Right, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1},
           KeyChord{XK_Up, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1},
           KeyChord{XK_Down, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1},
           KeyChord{XK_d, XCB_MOD_MASK_4},
           KeyChord{XK_Tab, XCB_MOD_MASK_1},
           KeyChord{XK_F4, XCB_MOD_MASK_1},
           KeyChord{XK_F7, XCB_MOD_MASK_1},
           KeyChord{XK_F8, XCB_MOD_MASK_1},
           KeyChord{XK_F9, XCB_MOD_MASK_1},
           KeyChord{XK_F10, XCB_MOD_MASK_1},
           KeyChord{XK_Tab, XCB_MOD_MASK_1 | XCB_MOD_MASK_SHIFT}}};
}

const char* shortcut_key(ShortcutAction action) { return keys[static_cast<std::size_t>(action)]; }
const char* shortcut_label(ShortcutAction action) { return labels[static_cast<std::size_t>(action)]; }

std::expected<std::optional<KeyChord>, std::string> parse_shortcut(std::string_view value) {
  value = trim(value);
  if (value.empty() || lowercase(value) == "none") return std::optional<KeyChord>{};
  std::uint16_t modifiers = 0;
  std::optional<std::uint32_t> key;
  while (!value.empty()) {
    const auto separator = value.find('+');
    const auto part = lowercase(trim(value.substr(0, separator)));
    if (part.empty()) return std::unexpected("Empty shortcut component");
    std::uint16_t modifier = 0;
    if (part == "ctrl" || part == "control") modifier = XCB_MOD_MASK_CONTROL;
    else if (part == "alt") modifier = XCB_MOD_MASK_1;
    else if (part == "super" || part == "meta" || part == "win") modifier = XCB_MOD_MASK_4;
    else if (part == "shift") modifier = XCB_MOD_MASK_SHIFT;
    if (modifier) {
      if (modifiers & modifier || key) return std::unexpected("Repeated or misplaced modifier");
      modifiers |= modifier;
    } else {
      if (key || separator != std::string_view::npos) return std::unexpected("Shortcut must end with one key");
      key = key_from_name(part);
      if (!key) return std::unexpected("Unknown key: " + part);
    }
    if (separator == std::string_view::npos) break;
    value.remove_prefix(separator + 1);
  }
  if (!key || !(modifiers & primary_modifiers))
    return std::unexpected("Use a key with Ctrl, Alt, or Super");
  return std::optional<KeyChord>{{*key, modifiers}};
}

std::string format_shortcut(std::optional<KeyChord> chord) {
  if (!chord) return "None";
  std::string result;
  if (chord->modifiers & XCB_MOD_MASK_CONTROL) result += "Ctrl+";
  if (chord->modifiers & XCB_MOD_MASK_1) result += "Alt+";
  if (chord->modifiers & XCB_MOD_MASK_4) result += "Super+";
  if (chord->modifiers & XCB_MOD_MASK_SHIFT) result += "Shift+";
  return result + name_for_key(chord->keysym);
}

bool shortcuts_conflict(const Shortcuts& shortcuts) {
  for (std::size_t i = 0; i < shortcuts.size(); ++i)
    for (std::size_t j = i + 1; j < shortcuts.size(); ++j)
      if (shortcuts[i] && shortcuts[j] && shortcuts[i] == shortcuts[j]) return true;
  return false;
}

std::optional<ShortcutAction> shortcut_action(const Shortcuts& shortcuts,
    std::uint32_t keysym, std::uint16_t modifiers) {
  if (keysym >= XK_A && keysym <= XK_Z) keysym = XK_a + keysym - XK_A;
  modifiers &= ~std::uint16_t(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2);
  if (modifiers & ~supported_modifiers) return std::nullopt;
  for (std::size_t i = 0; i < shortcuts.size(); ++i)
    if (shortcuts[i] && shortcuts[i]->keysym == keysym && shortcuts[i]->modifiers == modifiers)
      return static_cast<ShortcutAction>(i);
  return std::nullopt;
}

} // namespace midnight
