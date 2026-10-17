#pragma once

#include <array>
#include <string>

namespace midnight {

struct ThemeConfig {
  std::string panel_label{"Midnight ▾"};
  std::string clock_format{"%a %H:%M"};
  std::array<double, 3> desktop_color{.13, .38, .43};
  std::array<double, 3> panel_color{.70, .70, .70};
  std::array<double, 3> accent_color{.14, .27, .56};
};

[[nodiscard]] ThemeConfig load_config();

} // namespace midnight
