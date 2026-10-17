#include "config.hpp"

#include <glib.h>

#include <filesystem>
#include <optional>
#include <string_view>

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
  if (auto value = parse_color(read("desktop_color"))) config.desktop_color = *value;
  if (auto value = parse_color(read("panel_color"))) config.panel_color = *value;
  if (auto value = parse_color(read("accent_color"))) config.accent_color = *value;
  g_key_file_unref(keyfile);
  return config;
}
} // namespace midnight
