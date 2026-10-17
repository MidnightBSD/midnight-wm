// Copyright (c) 2026, Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cairo.h>

#include <filesystem>
#include <memory>

namespace midnight {

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
