#pragma once

#include <algorithm>
#include <cmath>

namespace midnight {

// Height is a useful default for ultrawide screens: 3440x1440 should have
// the same control size as 2560x1440. Users can override this in config.ini.
inline double display_scale(int screen_height, double override_scale = 0) {
  if (override_scale >= .75 && override_scale <= 3.0) return override_scale;
  if (screen_height >= 1800) return 2.0;
  if (screen_height >= 1300) return 1.25;
  return 1.0;
}

inline int scaled_pixel(double scale, int logical) {
  return std::max(1, static_cast<int>(std::lround(scale * logical)));
}

} // namespace midnight
