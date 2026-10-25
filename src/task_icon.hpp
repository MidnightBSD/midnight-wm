// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cairo/cairo.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace midnight {
using TaskIconSurface = std::shared_ptr<cairo_surface_t>;
struct TaskIcon {
  TaskIconSurface image;
  std::string wm_class;
  std::string instance;
  std::string theme_name;
  bool operator==(const TaskIcon&) const = default;
};

// Bounded _NET_WM_ICON decoding; retain at most a 64x64 premultiplied image.
[[nodiscard]] TaskIconSurface decode_task_icon(std::span<const std::uint32_t> data);
} // namespace midnight
