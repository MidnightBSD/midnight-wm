// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "desktop_layout.hpp"
#include <array>
#include <limits>

using midnight::DesktopLayout;
constexpr bool layout_checks() {
  constexpr auto parse = [](auto values) { return DesktopLayout::parse(values, 4); };
  const auto default_layout = DesktopLayout{};
  if (default_layout.neighbor(0, 4, true, false) != 3 ||
      default_layout.neighbor(3, 4, true, true) != 0 ||
      default_layout.neighbor(0, 4, false, true) != 0) return false;
  for (std::uint32_t orientation = 0; orientation < 2; ++orientation) {
    for (std::uint32_t corner = 0; corner < 4; ++corner) {
      const auto layout = parse(std::array{orientation, 2U, 2U, corner});
      if (!layout) return false;
      const auto origin = layout->position(0);
      if (!origin || origin->first != (corner == 1 || corner == 2) ||
          origin->second != (corner == 2 || corner == 3)) return false;
      for (std::uint32_t desktop = 0; desktop < 4; ++desktop) {
        const auto right = layout->neighbor(desktop, 4, true, true);
        const auto down = layout->neighbor(desktop, 4, false, true);
        if (right == desktop || down == desktop || right == down ||
            layout->neighbor(right, 4, true, false) != desktop ||
            layout->neighbor(down, 4, false, false) != desktop) return false;
      }
    }
  }
  const auto vertical = parse(std::array{1U, 2U, 2U, 0U});
  if (vertical->neighbor(0, 4, true, true) != 2 ||
      vertical->neighbor(0, 4, false, true) != 1) return false;
  const auto legacy = parse(std::array{0U, 2U, 0U});
  if (!legacy || legacy->corner || legacy->rows != 2) return false;
  const auto derived = parse(std::array{1U, 0U, 3U, 0U});
  if (!derived || derived->columns != 2 || derived->neighbor(3, 4, false, true) != 3 ||
      derived->neighbor(0, 4, true, true) != 3) return false;
  const auto incomplete = parse(std::array{0U, 3U, 2U, 0U});
  if (!incomplete || incomplete->neighbor(3, 4, true, true) != 3 ||
      incomplete->neighbor(1, 4, false, true) != 1) return false;
  const auto small = parse(std::array{0U, 1U, 1U, 0U});
  if (small) return false;
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  const auto enormous = parse(std::array{1U, maximum, maximum, 2U});
  if (!enormous || enormous->neighbor(0, 4, false, true) != 3 ||
      enormous->neighbor(0, 4, true, true) != 0) return false;
  return !parse(std::array{0U, 0U, 0U, 0U}) &&
         !parse(std::array{2U, 2U, 2U, 0U}) &&
         !parse(std::array{0U, 2U, 2U, 4U}) &&
         !parse(std::array{0U, 2U}) &&
         !parse(std::array{0U, 2U, 2U, 0U, 0U}) &&
         !DesktopLayout::parse(std::array{0U, 2U, 2U, 0U}, 0);
}
static_assert(layout_checks());
int main() { return layout_checks() ? 0 : 1; }
