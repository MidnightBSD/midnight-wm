// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <bit>
#include <cstdint>

namespace midnight {
// X server timestamps wrap every 2^32 milliseconds. Compare differences as
// signed values, assuming timestamps are less than half the range apart.
constexpr bool timestamp_before(std::uint32_t first, std::uint32_t second) {
  return std::bit_cast<std::int32_t>(std::uint32_t(first - second)) < 0;
}
}
