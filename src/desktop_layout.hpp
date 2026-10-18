// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace midnight {
// _NET_DESKTOP_LAYOUT is supplied by the pager. Coordinates are physical,
// regardless of the order in which the pager numbers its cells.
struct DesktopLayout {
  std::uint32_t orientation{}, columns{4}, rows{1}, corner{};

  static constexpr std::optional<DesktopLayout> parse(
      std::span<const std::uint32_t> values, std::uint32_t count) {
    if ((values.size() != 3 && values.size() != 4) || !count || values[0] > 1 ||
        (!values[1] && !values[2]) || (values.size() == 4 && values[3] > 3)) return {};
    DesktopLayout result{values[0], values[1], values[2], values.size() == 4 ? values[3] : 0};
    if (!result.columns) result.columns = 1 + (count - 1) / result.rows;
    if (!result.rows) result.rows = 1 + (count - 1) / result.columns;
    if (std::uint64_t(result.columns) * result.rows < count) return {};
    return result;
  }

  constexpr std::optional<std::pair<std::uint32_t, std::uint32_t>> position(
      std::uint32_t desktop) const {
    if (!columns || !rows || std::uint64_t(desktop) >= std::uint64_t(columns) * rows) return {};
    auto x = orientation ? desktop / rows : desktop % columns;
    auto y = orientation ? desktop % rows : desktop / columns;
    if (corner == 1 || corner == 2) x = columns - 1 - x;
    if (corner == 2 || corner == 3) y = rows - 1 - y;
    return std::pair{x, y};
  }

  // Wrap within a row or column, skipping empty cells. Scan desktops rather
  // than cells so even a pager's enormous CARDINAL dimensions remain bounded.
  constexpr std::uint32_t neighbor(std::uint32_t desktop, std::uint32_t count,
                                   bool horizontal, bool forward) const {
    const auto from = position(desktop);
    if (!from || desktop >= count) return desktop;
    const auto extent = horizontal ? columns : rows;
    const auto start = horizontal ? from->first : from->second;
    std::uint32_t best = desktop;
    std::uint64_t distance = extent;
    for (std::uint32_t candidate = 0; candidate < count; ++candidate) {
      const auto to = position(candidate);
      if (!to || candidate == desktop ||
          (horizontal ? to->second != from->second : to->first != from->first)) continue;
      const auto end = horizontal ? to->first : to->second;
      const auto delta = forward ? (std::uint64_t(end) + extent - start) % extent
                                 : (std::uint64_t(start) + extent - end) % extent;
      if (delta < distance) { distance = delta; best = candidate; }
    }
    return best;
  }
};
}
