#pragma once

#include "shortcuts.hpp"

#include <array>
#include <string>
#include <string_view>
#include <optional>

namespace midnight {

// Wallpaper used when desktop_image is absent, from midnightbsd-backgrounds.
inline constexpr const char* default_desktop_image = "mnbsd_bg_purple.png";

struct ThemeConfig {
  Shortcuts shortcuts{default_shortcuts()};
  std::string panel_label{"Midnight ▾"};
  std::string clock_format{"%a %H:%M"};
  bool dark_mode{};
  bool reduced_motion{};
  bool show_battery{true};
  bool compositing{true};
  bool restore_session{};
  bool save_session{};
  bool save_application_checkpoints{};
  bool restore_applications{};
  bool restore_desktop_applications{};
  bool start_xscreensaver{true};
  std::string ui_font{"Sans"};
  std::string title_font{"Sans Bold"};
  std::string desktop_image;
  std::string desktop_image_mode{"fill"};
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
// Parse a complete snapshot; malformed INI data must not replace live settings.
[[nodiscard]] std::optional<ThemeConfig> parse_config(std::string_view contents);

} // namespace midnight
