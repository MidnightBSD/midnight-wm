// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace midnight {

// The percentage one brightness key press should select. Drivers that publish
// discrete levels move to the neighbouring level; others move by `step`. The
// result never reaches zero, so the brightness-down key cannot blank a panel
// whose driver treats zero as off.
[[nodiscard]] int next_backlight_level(int current, std::span<const std::uint32_t> levels,
                                       bool up, int step = 5);

// Controls the panel backlight through backlight(9), like backlight(8). It
// has no X11 dependency and runs no commands. The device node is normally
// writable by the video group.
class BacklightControl {
public:
  explicit BacklightControl(std::string device = {});
  // Current brightness, 0-100.
  [[nodiscard]] std::expected<int, std::string> read() const;
  // Step one level up or down; returns the new brightness.
  [[nodiscard]] std::expected<int, std::string> adjust(bool up) const;

private:
  [[nodiscard]] std::expected<int, std::string> open_device(bool writable) const;
  std::string device_;
};

} // namespace midnight
