#pragma once

#include <array>
#include <string>

namespace midnight {

struct ThemeConfig {
  std::string panel_label{"Midnight ▾"};
  std::string clock_format{"%a %H:%M"};
  std::array<double, 3> desktop_color{.04, .05, .48};
  std::array<double, 3> panel_color{.77, .77, .77};
  std::array<double, 3> accent_color{.12, .12, .55};
};

[[nodiscard]] ThemeConfig load_config();

} // namespace midnight
