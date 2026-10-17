// Copyright (c) 2026, Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "wallpaper.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

namespace midnight {
namespace {
constexpr int maximum_axis = 8192;
constexpr std::int64_t maximum_pixels = 32 * 1024 * 1024;
using Pixbuf = std::unique_ptr<GdkPixbuf, decltype(&g_object_unref)>;

bool valid_dimensions(int width, int height) {
  return width > 0 && height > 0 && width <= maximum_axis &&
      height <= maximum_axis &&
      static_cast<std::int64_t>(width) * height <= maximum_pixels;
}
} // namespace

bool Wallpaper::load(const std::filesystem::path& path,
                     int target_width, int target_height) {
  surface_.reset();
  if (path.empty() || target_width <= 0 || target_height <= 0) return false;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) return false;

  int source_width{}, source_height{};
  GdkPixbufFormat* format = gdk_pixbuf_get_file_info(
      path.c_str(), &source_width, &source_height);
  if (!format || source_width <= 0 || source_height <= 0) return false;

  // Decode enough pixels for a centered cover image, then cap the dimensions
  // and allocation. Small raster images can be enlarged when painting instead.
  double scale = std::max(static_cast<double>(target_width) / source_width,
                          static_cast<double>(target_height) / source_height);
  if (!gdk_pixbuf_format_is_scalable(format)) scale = std::min(scale, 1.0);
  scale = std::min({scale, static_cast<double>(maximum_axis) / source_width,
                    static_cast<double>(maximum_axis) / source_height,
                    std::sqrt(static_cast<double>(maximum_pixels) /
                              (static_cast<double>(source_width) * source_height))});
  const int requested_width = std::max(1, static_cast<int>(source_width * scale));
  const int requested_height = std::max(1, static_cast<int>(source_height * scale));
  // GdkPixbuf returns the static first frame for animated formats such as GIF.
  Pixbuf image(gdk_pixbuf_new_from_file_at_scale(path.c_str(), requested_width,
                  requested_height, TRUE, nullptr), g_object_unref);
  if (!image) return false;
  const int width = gdk_pixbuf_get_width(image.get());
  const int height = gdk_pixbuf_get_height(image.get());
  const int channels = gdk_pixbuf_get_n_channels(image.get());
  const bool alpha = gdk_pixbuf_get_has_alpha(image.get());
  if (!valid_dimensions(width, height) ||
      gdk_pixbuf_get_colorspace(image.get()) != GDK_COLORSPACE_RGB ||
      gdk_pixbuf_get_bits_per_sample(image.get()) != 8 ||
      channels != (alpha ? 4 : 3)) return false;

  decltype(surface_) surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       width, height));
  if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) return false;
  cairo_surface_flush(surface.get());
  auto* destination = cairo_image_surface_get_data(surface.get());
  const int destination_stride = cairo_image_surface_get_stride(surface.get());
  const auto* source = gdk_pixbuf_read_pixels(image.get());
  const int source_stride = gdk_pixbuf_get_rowstride(image.get());
  for (int y = 0; y < height; ++y) {
    const auto* input = source + static_cast<std::size_t>(y) * source_stride;
    auto* output = reinterpret_cast<std::uint32_t*>(
        destination + static_cast<std::size_t>(y) * destination_stride);
    for (int x = 0; x < width; ++x, input += channels) {
      const std::uint32_t a = alpha ? input[3] : 255;
      const auto premultiply = [a](std::uint32_t value) {
        return (value * a + 127) / 255;
      };
      output[x] = (a << 24) | (premultiply(input[0]) << 16) |
                  (premultiply(input[1]) << 8) | premultiply(input[2]);
    }
  }
  cairo_surface_mark_dirty(surface.get());
  surface_ = std::move(surface);
  return true;
}

void Wallpaper::paint(cairo_t* cr, double x, double y, double width,
                      double height, bool fill) const {
  if (!surface_ || !cr || width <= 0 || height <= 0 ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
      !std::isfinite(height)) return;
  const double image_width = cairo_image_surface_get_width(surface_.get());
  const double image_height = cairo_image_surface_get_height(surface_.get());
  const double scale = fill ? std::max(width / image_width, height / image_height)
                            : std::min(width / image_width, height / image_height);
  cairo_save(cr);
  cairo_rectangle(cr, x, y, width, height);
  cairo_clip(cr);
  cairo_translate(cr, x + (width - image_width * scale) / 2,
                       y + (height - image_height * scale) / 2);
  cairo_scale(cr, scale, scale);
  cairo_rectangle(cr, 0, 0, image_width, image_height);
  cairo_clip(cr);
  cairo_set_source_surface(cr, surface_.get(), 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  cairo_paint(cr);
  cairo_restore(cr);
}

} // namespace midnight
