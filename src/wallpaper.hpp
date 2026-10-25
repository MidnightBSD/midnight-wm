// Copyright (c) 2026, Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cairo.h>

#include <filesystem>
#include <memory>

namespace midnight {

// Bounds for decoding untrusted raster images inside the desktop process.
// Most GdkPixbuf loaders allocate the full declared size before scaling, so
// check the header dimensions before decoding.
inline constexpr int maximum_image_axis = 8192;
inline constexpr long long maximum_image_pixels = 32LL * 1024 * 1024;

[[nodiscard]] constexpr bool safe_image_dimensions(int width, int height) {
  return width > 0 && height > 0 && width <= maximum_image_axis &&
      height <= maximum_image_axis &&
      static_cast<long long>(width) * height <= maximum_image_pixels;
}

// A static image cached as a premultiplied Cairo surface, independent of X11.
class Wallpaper {
public:
  Wallpaper() = default;
  Wallpaper(const Wallpaper&) = delete;
  Wallpaper& operator=(const Wallpaper&) = delete;

  [[nodiscard]] bool load(const std::filesystem::path& path,
                          int target_width, int target_height);
  void paint(cairo_t* cr, double x, double y, double width, double height,
             bool fill) const;

private:
  struct SurfaceDeleter {
    void operator()(cairo_surface_t* surface) const noexcept {
      cairo_surface_destroy(surface);
    }
  };
  std::unique_ptr<cairo_surface_t, SurfaceDeleter> surface_;
};

} // namespace midnight
