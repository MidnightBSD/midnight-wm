// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "task_icon.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace midnight {
TaskIconSurface decode_task_icon(std::span<const std::uint32_t> data) {
  if (data.size() > 1024 * 1024) return {};
  std::size_t best = 0;
  std::uint32_t width = 0, height = 0;
  auto best_score = std::numeric_limits<std::uint32_t>::max();
  for (std::size_t offset = 0; data.size() - offset >= 2;) {
    const auto w = data[offset++], h = data[offset++];
    if (!w || !h || w > 4096 || h > 4096) break;
    const auto count = std::uint64_t(w) * h;
    if (count > data.size() - offset) break;
    const auto extent = std::max(w, h);
    const auto score = extent >= 64 ? extent - 64 : 4096 + 64 - extent;
    if (score < best_score) {
      best = offset;
      width = w;
      height = h;
      best_score = score;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (!width) return {};
  TaskIconSurface source(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height), cairo_surface_destroy);
  if (cairo_surface_status(source.get()) != CAIRO_STATUS_SUCCESS) return {};
  auto* pixels = cairo_image_surface_get_data(source.get());
  const auto stride = cairo_image_surface_get_stride(source.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    auto* row = reinterpret_cast<std::uint32_t*>(pixels + y * stride);
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto argb = data[best + std::size_t(y) * width + x];
      const auto a = argb >> 24;
      row[x] = (a << 24) | ((((argb >> 16) & 255) * a / 255) << 16) |
               ((((argb >> 8) & 255) * a / 255) << 8) | ((argb & 255) * a / 255);
    }
  }
  cairo_surface_mark_dirty(source.get());
  if (width <= 64 && height <= 64) return source;
  const double factor = 64.0 / std::max(width, height);
  const int output_width = std::max(1, static_cast<int>(std::lround(width * factor)));
  const int output_height = std::max(1, static_cast<int>(std::lround(height * factor)));
  TaskIconSurface output(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, output_width, output_height), cairo_surface_destroy);
  if (cairo_surface_status(output.get()) != CAIRO_STATUS_SUCCESS) return {};
  auto* cr = cairo_create(output.get());
  cairo_scale(cr, output_width / double(width), output_height / double(height));
  cairo_set_source_surface(cr, source.get(), 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_paint(cr);
  const bool valid = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
  cairo_destroy(cr);
  return valid ? output : TaskIconSurface{};
}
} // namespace midnight
