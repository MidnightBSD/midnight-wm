#include "config.hpp"

#include <glib.h>

#include <filesystem>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#ifndef MIDNIGHT_WM_BACKGROUND_DIR
#define MIDNIGHT_WM_BACKGROUND_DIR "/usr/local/share/backgrounds/midnightbsd"
#endif

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

std::optional<ThemeConfig> parse_config(std::string_view contents) {
  ThemeConfig config;
  const auto path = std::filesystem::path(g_get_user_config_dir()) / "midnight-wm" / "config.ini";
  auto* keyfile = g_key_file_new();
  if (!g_key_file_load_from_data(keyfile, contents.data(), contents.size(), G_KEY_FILE_NONE, nullptr)) {
    g_key_file_unref(keyfile);
    return std::nullopt;
  }
  auto read = [keyfile](const char* key) -> std::string {
    char* value = g_key_file_get_string(keyfile, "Workspace", key, nullptr);
    std::string result = value ? value : "";
    g_free(value);
    return result;
  };
  if (auto value = read("panel_label"); !value.empty()) config.panel_label = std::move(value);
  if (auto value = read("clock_format"); !value.empty()) config.clock_format = std::move(value);
  // Relative filenames resolve against the config directory, then installed
  // background directories. Returns nothing when no candidate exists.
  auto find_image = [&path](const std::string& value) -> std::optional<std::filesystem::path> {
    std::error_code error;
    const auto local = path.parent_path() / value;
    if (std::filesystem::is_regular_file(local, error)) return local;
    std::vector<std::filesystem::path> directories{
      std::filesystem::path(g_get_user_data_dir()) / "backgrounds"};
    for (const char* const* data = g_get_system_data_dirs(); *data; ++data) {
      directories.emplace_back(std::filesystem::path(*data) / "backgrounds/midnightbsd");
      directories.emplace_back(std::filesystem::path(*data) / "backgrounds");
    }
    directories.emplace_back(MIDNIGHT_WM_BACKGROUND_DIR);
    for (const auto& directory : directories) {
      const auto candidate = directory / value;
      if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }
    return std::nullopt;
  };
  if (!g_key_file_has_key(keyfile, "Workspace", "desktop_image", nullptr)) {
    // Without an explicit setting, use the midnightbsd-backgrounds wallpaper
    // when installed; otherwise desktop_color remains the background.
    if (auto image = find_image(default_desktop_image))
      config.desktop_image = image->lexically_normal().string();
  } else if (auto value = read("desktop_image"); !value.empty()) {
    std::filesystem::path image(value);
    if (value.starts_with("~/")) image = std::filesystem::path(g_get_home_dir()) / value.substr(2);
    if (image.is_relative()) image = find_image(value).value_or(path.parent_path() / image);
    config.desktop_image = image.lexically_normal().string();
  }
  if (auto value = read("desktop_image_mode"); value == "fit" || value == "fill")
    config.desktop_image_mode = std::move(value);
  GError* battery_error = nullptr;
  const bool show_battery = g_key_file_get_boolean(keyfile, "Workspace", "show_battery", &battery_error);
  if (battery_error) g_error_free(battery_error);
  else config.show_battery = show_battery;
  GError* compositor_error = nullptr;
  const bool compositing = g_key_file_get_boolean(keyfile, "Workspace", "compositing", &compositor_error);
  if (compositor_error) g_error_free(compositor_error);
  else config.compositing = compositing;
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
  config.restore_session = g_key_file_get_boolean(keyfile, "Session", "restore_folders", nullptr);
  config.save_session = g_key_file_get_boolean(keyfile, "Session", "save_on_logout", nullptr);
  g_key_file_unref(keyfile);
  return config;
}
ThemeConfig load_config() {
  const auto path = std::filesystem::path(g_get_user_config_dir()) / "midnight-wm" / "config.ini";
  char* contents = nullptr;
  gsize length{};
  // An empty snapshot still applies defaults that depend on installed files.
  if (!g_file_get_contents(path.c_str(), &contents, &length, nullptr))
    return parse_config("").value_or(ThemeConfig{});
  const auto config = parse_config(std::string_view(contents, length));
  g_free(contents);
  return config ? *config : parse_config("").value_or(ThemeConfig{});
}
} // namespace midnight
