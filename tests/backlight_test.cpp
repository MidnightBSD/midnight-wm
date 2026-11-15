// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include "backlight.hpp"

#include <array>
#include <cstdint>
#include <iostream>

int main() {
  using midnight::next_backlight_level;
  bool passed = true;
  const auto check = [&](int actual, int expected, const char* name) {
    if (actual == expected) return;
    std::cerr << name << ": " << actual << " instead of " << expected << '\n';
    passed = false;
  };
  check(next_backlight_level(50, {}, true), 55, "plain step up");
  check(next_backlight_level(50, {}, false), 45, "plain step down");
  check(next_backlight_level(98, {}, true), 100, "clamps at full");
  check(next_backlight_level(3, {}, false), 1, "never reaches zero");
  check(next_backlight_level(0, {}, false), 1, "leaves zero");
  check(next_backlight_level(500, {}, true), 100, "out-of-range input");
  const std::array<std::uint32_t, 5> levels{100, 0, 60, 20, 80};
  check(next_backlight_level(60, levels, true), 80, "next level");
  check(next_backlight_level(60, levels, false), 20, "previous level");
  check(next_backlight_level(70, levels, false), 60, "between levels down");
  check(next_backlight_level(70, levels, true), 80, "between levels up");
  check(next_backlight_level(100, levels, true), 100, "top level");
  check(next_backlight_level(20, levels, false), 20, "bottom level skips zero");
  const std::array<std::uint32_t, 3> hostile{0, 4000000000u, 250};
  check(next_backlight_level(50, hostile, true), 55, "invalid levels fall back to steps");
  // A missing device reports an error rather than inventing a value.
  const midnight::BacklightControl missing("/nonexistent/backlight");
  if (missing.read() || missing.adjust(true)) { std::cerr << "missing device succeeded\n"; passed = false; }
  return passed ? 0 : 1;
}
