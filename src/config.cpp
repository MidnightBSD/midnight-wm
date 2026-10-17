#include "config.hpp"

#include <glib.h>

#include <filesystem>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

namespace midnight {
namespace {
std::optional<std::array<double, 3>> parse_color(std::string_view value) {
  if (value.size() != 7 || value.front() != '#') return std::nullopt;
  auto digit = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::array<double, 3> result{};
  for (int i = 0; i < 3; ++i) {
    const int high = digit(value[1 + i * 2]);
    const int low = digit(value[2 + i * 2]);
    if (high < 0 || low < 0) return std::nullopt;
    result[i] = (high * 16 + low) / 255.0;
  }
  return result;
}
} // namespace

ThemeConfig load_config() {
  ThemeConfig config;
  const auto path = std::filesystem::path(g_get_user_config_dir()) / "midnight-wm" / "config.ini";
  auto* keyfile = g_key_file_new();
  if (!g_key_file_load_from_file(keyfile, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
    g_key_file_unref(keyfile);
    return config;
  }
  auto read = [keyfile](const char* key) -> std::string {
    char* value = g_key_file_get_string(keyfile, "Workspace", key, nullptr);
    std::string result = value ? value : "";
    g_free(value);
    return result;
  };
  if (auto value = read("panel_label"); !value.empty()) config.panel_label = std::move(value);
  if (auto value = read("clock_format"); !value.empty()) config.clock_format = std::move(value);
  config.dark_mode = g_key_file_get_boolean(keyfile, "Workspace", "dark_mode", nullptr);
  if (config.dark_mode) {
    config.panel_color = {.18, .18, .21};
    config.text_color = {.92, .92, .95};
    config.surface_color = {.18, .18, .21};
    config.field_color = {.11, .11, .14};
    config.button_color = {.25, .25, .29};
    config.highlight_color = {.40, .40, .45};
    config.shadow_color = {.07, .07, .09};
    config.muted_text_color = {.65, .65, .69};
  }
  for (auto [key, target] : {std::pair{"ui_font", &config.ui_font},
                             std::pair{"title_font", &config.title_font}}) {
    if (auto value = read(key); !value.empty() && value.size() <= 256 &&
        g_utf8_validate(value.c_str(), value.size(), nullptr)) *target = std::move(value);
  }
  if (auto value = parse_color(read("desktop_color"))) config.desktop_color = *value;
  if (auto value = parse_color(read("panel_color"))) config.panel_color = *value;
  if (auto value = parse_color(read("accent_color"))) config.accent_color = *value;
  if (auto value = read("ui_scale"); !value.empty()) {
    char* end = nullptr;
    const double scale = g_ascii_strtod(value.c_str(), &end);
    if (end && *end == '\0' && std::isfinite(scale) && scale >= .75 && scale <= 3.0)
      config.ui_scale = scale;
  }
  auto read_desktop_bool = [keyfile](const char* key, bool& value) {
    GError* error = nullptr;
    const bool configured = g_key_file_get_boolean(keyfile, "Desktop", key, &error);
    if (error) g_error_free(error);
    else value = configured;
  };
  read_desktop_bool("show_documents", config.show_documents);
  read_desktop_bool("show_pictures", config.show_pictures);
  read_desktop_bool("show_music", config.show_music);
  read_desktop_bool("show_videos", config.show_videos);
  read_desktop_bool("show_public_share", config.show_public_share);
  g_key_file_unref(keyfile);
  return config;
}
} // namespace midnight
