// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace midnight {

// Helpers for the Xft.dpi entry in the root window's RESOURCE_MANAGER text,
// the xrdb database that Xft clients such as xterm read when they start.
// Every other line is preserved byte for byte.

namespace x_resources_detail {
constexpr std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
    value.remove_suffix(1);
  return value;
}

constexpr bool is_xft_dpi(std::string_view line) {
  const auto colon = line.find(':');
  return colon != std::string_view::npos && trim(line.substr(0, colon)) == "Xft.dpi";
}
} // namespace x_resources_detail

// The integer Xft.dpi value, if a line sets one. The last such line wins.
constexpr std::optional<int> xft_dpi_resource(std::string_view resources) {
  std::optional<int> result;
  while (!resources.empty()) {
    const auto end = resources.find('\n');
    const auto line = resources.substr(0, end);
    if (x_resources_detail::is_xft_dpi(line)) {
      const auto value = x_resources_detail::trim(line.substr(line.find(':') + 1));
      int dpi = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), dpi);
      result = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && dpi > 0
                   ? std::optional<int>(dpi) : std::nullopt;
    }
    if (end == std::string_view::npos) break;
    resources.remove_prefix(end + 1);
  }
  return result;
}

// Replace every Xft.dpi line with one entry, or remove them when dpi is empty.
inline std::string with_xft_dpi(std::string_view resources, std::optional<int> dpi) {
  std::string result;
  result.reserve(resources.size() + 24);
  while (!resources.empty()) {
    const auto end = resources.find('\n');
    const auto line = resources.substr(0, end);
    if (!x_resources_detail::is_xft_dpi(line)) {
      result += line;
      result += '\n';
    }
    if (end == std::string_view::npos) break;
    resources.remove_prefix(end + 1);
  }
  if (dpi) result += "Xft.dpi:\t" + std::to_string(*dpi) + "\n";
  return result;
}

// Whether the desktop may set Xft.dpi: when nothing sets it, when the value is
// the one this desktop wrote earlier, or when it is the 96 DPI default. Any
// other value is the user's own (~/.Xresources) and stays.
constexpr bool may_manage_xft_dpi(std::optional<int> current, std::optional<int> written) {
  return !current || *current == 96 || (written && *current == *written);
}

} // namespace midnight
