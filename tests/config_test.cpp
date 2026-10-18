#include "config.hpp"
#include "ui_theme.hpp"
#include <glib.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>

int main() {
  char* temporary = g_dir_make_tmp("midnight-theme-test-XXXXXX", nullptr);
  if (!temporary) return 1;
  const std::filesystem::path root(temporary);
  g_free(temporary);
  g_setenv("XDG_CONFIG_HOME", root.c_str(), true);
  g_setenv("XDG_DATA_HOME", (root / "data").c_str(), true);
  g_setenv("XDG_DATA_DIRS", (root / "system").c_str(), true);
  std::filesystem::create_directories(root / "midnight-wm");
  const auto write = [&](const char* content) {
    std::ofstream(root / "midnight-wm/config.ini") << content;
  };
  bool passed = true;
  const auto check = [&](bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; passed = false; }
  };
  check(!midnight::parse_config("[Workspace\ndark_mode=true"), "malformed snapshot must be rejected");
  check(midnight::parse_config("[Workspace]\ndark_mode=true\n")->dark_mode, "complete snapshot must parse");
  const auto defaults = midnight::load_config();
  check(!defaults.dark_mode && defaults.ui_font == "Sans" && defaults.title_font == "Sans Bold",
        "missing config must preserve default fonts and light mode");
  check(defaults.compositing, "compositing must default to enabled");
  check(!midnight::parse_config("[Workspace]\ncompositing=false\n")->compositing,
        "compositing can be disabled");
  check(midnight::parse_config("[Workspace]\ncompositing=invalid\n")->compositing,
        "invalid compositing value must retain default");
  check(defaults.show_battery, "battery indicator must default to enabled");
  check(!midnight::parse_config("[Workspace]\nshow_battery=false\n")->show_battery,
        "battery indicator can be disabled");
  check(midnight::parse_config("[Workspace]\nshow_battery=invalid\n")->show_battery,
        "invalid battery setting must retain enabled default");
  write("[Workspace]\ndark_mode=true\nui_font=DejaVu Sans 12\ntitle_font=DejaVu Serif Bold 14\n");
  const auto dark = midnight::load_config();
  check(dark.dark_mode && dark.panel_color[0] < .3 && dark.text_color[0] > .8,
        "dark palette must have contrasting text and surfaces");
  check(dark.desktop_color == defaults.desktop_color, "dark mode must preserve desktop background default");
  auto* font = midnight::make_ui_font(dark, false, 13, true);
  check(std::abs(pango_font_description_get_size(font) / double(PANGO_SCALE) - 16) < .01,
        "configured point size must convert to logical pixels");
  pango_font_description_free(font);
  font = midnight::make_ui_font(defaults, false, 13, true);
  check(pango_font_description_get_size(font) == 13 * PANGO_SCALE,
        "unsized font must preserve default widget size");
  pango_font_description_free(font);
  write("[Workspace]\ndark_mode=true\ndesktop_color=#123456\npanel_color=#abcdef\n");
  const auto custom = midnight::load_config();
  check(std::abs(custom.desktop_color[0] - 18.0 / 255) < .001 &&
        std::abs(custom.panel_color[0] - 171.0 / 255) < .001,
        "explicit colors must override palette defaults");
  write("[Workspace]\ndark_mode=invalid\ndesktop_color=wrong\nui_font=\n");
  const auto invalid = midnight::load_config();
  check(!invalid.dark_mode && invalid.desktop_color == defaults.desktop_color &&
        invalid.ui_font == defaults.ui_font, "invalid values must retain defaults");
  check((defaults.desktop_image.empty() ||
         std::filesystem::path(defaults.desktop_image).filename() == midnight::default_desktop_image) &&
        defaults.desktop_image_mode == "fill",
        "default wallpaper must be the installed MidnightBSD background or none");
  std::filesystem::create_directories(root / "system/backgrounds/midnightbsd");
  std::ofstream(root / "system/backgrounds/midnightbsd" / midnight::default_desktop_image) << "fixture";
  check(midnight::load_config().desktop_image ==
        (root / "system/backgrounds/midnightbsd" / midnight::default_desktop_image).string(),
        "missing config must use the installed default wallpaper");
  write("[Workspace]\ndark_mode=true\n");
  check(std::filesystem::path(midnight::load_config().desktop_image).filename() == midnight::default_desktop_image,
        "unset desktop_image must use the default wallpaper");
  write("[Workspace]\ndesktop_image=\n");
  check(midnight::load_config().desktop_image.empty(), "empty desktop_image must disable the wallpaper");
  std::ofstream(root / "system/backgrounds/midnightbsd/example.png") << "fixture";
  write("[Workspace]\ndesktop_image=example.png\ndesktop_image_mode=fit\n");
  auto image_config = midnight::load_config();
  check(image_config.desktop_image == (root / "system/backgrounds/midnightbsd/example.png").string() &&
        image_config.desktop_image_mode == "fit", "installed background search path must resolve filenames");
  std::ofstream(root / "midnight-wm/example.png") << "local fixture";
  check(midnight::load_config().desktop_image == (root / "midnight-wm/example.png").string(),
        "config-relative image must take priority over installed images");
  write("[Workspace]\ndesktop_image=~/Pictures/background.svg\ndesktop_image_mode=invalid\n");
  image_config = midnight::load_config();
  check(image_config.desktop_image == (std::filesystem::path(g_get_home_dir()) / "Pictures/background.svg").string() &&
        image_config.desktop_image_mode == "fill", "home expansion and invalid mode fallback");
  std::filesystem::remove_all(root);
  return passed ? 0 : 1;
}
