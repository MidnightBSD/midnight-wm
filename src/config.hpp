#pragma once

#include <array>
#include <string>

namespace midnight {

struct ThemeConfig {
  std::string panel_label{"Midnight ▾"};
  std::string clock_format{"%a %H:%M"};
  bool dark_mode{};
  std::string ui_font{"Sans"};
  std::string title_font{"Sans Bold"};
  std::array<double, 3> text_color{.04, .04, .04};
  std::array<double, 3> surface_color{.75, .75, .75};
  std::array<double, 3> field_color{.96, .96, .92};
  std::array<double, 3> button_color{.80, .80, .80};
  std::array<double, 3> highlight_color{1, 1, 1};
  std::array<double, 3> shadow_color{.25, .25, .25};
  std::array<double, 3> muted_text_color{.48, .48, .48};
  std::array<double, 3> desktop_color{.47, .40, .66};
  std::array<double, 3> panel_color{.77, .77, .77};
  std::array<double, 3> accent_color{59.0 / 255.0, 32.0 / 255.0, 95.0 / 255.0};
  double ui_scale{}; // Zero selects a scale from the screen height.
  bool show_documents{true};
  bool show_pictures{true};
  bool show_music{};
  bool show_videos{};
  bool show_public_share{};
};

[[nodiscard]] ThemeConfig load_config();

} // namespace midnight
