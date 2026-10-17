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
  std::filesystem::create_directories(root / "midnight-wm");
  const auto write = [&](const char* content) {
    std::ofstream(root / "midnight-wm/config.ini") << content;
  };
  bool passed = true;
  const auto check = [&](bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; passed = false; }
  };
  const auto defaults = midnight::load_config();
  check(!defaults.dark_mode && defaults.ui_font == "Sans" && defaults.title_font == "Sans Bold",
        "missing config must preserve default fonts and light mode");
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
  std::filesystem::remove_all(root);
  return passed ? 0 : 1;
}
