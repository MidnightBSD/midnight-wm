#pragma once

#include <algorithm>
#include <cmath>

namespace midnight {

// Pixel density from the monitor's reported physical size, or zero when the
// size is missing or implausible. The diagonal keeps rotated outputs correct.
// Projectors and televisions often report their aspect ratio (160x90 mm, 16x9
// mm) instead of a size, and anything under four inches is not a real panel.
constexpr int display_dpi(int width, int height, int width_mm, int height_mm) {
  if (width <= 0 || height <= 0 || width_mm <= 0 || height_mm <= 0) return 0;
  if ((width_mm == 160 && (height_mm == 90 || height_mm == 100)) ||
      (width_mm == 90 && height_mm == 160) || (width_mm == 100 && height_mm == 160)) return 0;
  const long long pixels = 1LL * width * width + 1LL * height * height;
  const long long size = 1LL * width_mm * width_mm + 1LL * height_mm * height_mm;
  if (size < 100LL * 100) return 0;
  // dpi^2 = pixels * 25.4^2 / size; find the integer root without <cmath>.
  const long long squared = pixels * 64516 / (size * 100);
  if (squared > 1000LL * 1000) return 0;
  int dpi = 0;
  while (1LL * (dpi + 1) * (dpi + 1) <= squared) ++dpi;
  return dpi >= 40 ? dpi : 0;
}

// Height is a useful default for ultrawide screens: 3440x1440 should have
// the same control size as 2560x1440. Dense laptop panels need more than
// their height suggests (2256x1504 at 13.5 inches is about 200 DPI), so a
// known pixel density can raise the scale but never lowers it.
constexpr double automatic_display_scale(int screen_height, int dpi = 0) {
  const double by_height = screen_height >= 1800 ? 2.0 : screen_height >= 1300 ? 1.25 : 1.0;
  const double by_density = dpi >= 240 ? 2.0 : dpi >= 170 ? 1.5 : dpi >= 150 ? 1.25 : 1.0;
  return std::max(by_height, by_density);
}

// Users can override the automatic choice in config.ini.
constexpr double display_scale(int screen_height, double override_scale = 0, int dpi = 0) {
  if (override_scale >= .75 && override_scale <= 3.0) return override_scale;
  return automatic_display_scale(screen_height, dpi);
}

inline int scaled_pixel(double scale, int logical) {
  return std::max(1, static_cast<int>(std::lround(scale * logical)));
}

} // namespace midnight
