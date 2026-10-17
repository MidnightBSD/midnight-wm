#include "browser_launcher.hpp"

#include "startup_notification.hpp"

#include <gio/gdesktopappinfo.h>
#include <glib.h>

#include <filesystem>
#include <string>
#include <vector>

namespace midnight {
namespace {
std::vector<std::string> desktop_mimeapps_names() {
  std::vector<std::string> names;
  const char* desktops = g_getenv("XDG_CURRENT_DESKTOP");
  if (desktops) {
    char** parts = g_strsplit(desktops, ":", -1);
    for (char** part = parts; part && *part; ++part) {
      if (!**part) continue;
      char* lower = g_ascii_strdown(*part, -1);
      names.emplace_back(std::string(lower) + "-mimeapps.list");
      g_free(lower);
    }
    g_strfreev(parts);
  }
  names.emplace_back("mimeapps.list");
  return names;
}

bool has_default(const std::filesystem::path& path, const char* scheme) {
  auto* file = g_key_file_new();
  if (!g_key_file_load_from_file(file, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
    g_key_file_unref(file);
    return false;
  }
  const auto key = std::string("x-scheme-handler/") + scheme;
  gsize count = 0;
  char** entries = g_key_file_get_string_list(file, "Default Applications", key.c_str(), &count, nullptr);
  bool found = false;
  for (gsize i = 0; i < count; ++i)
    if (entries[i] && *entries[i]) { found = true; break; }
  g_strfreev(entries);
  g_key_file_unref(file);
  return found;
}

bool configured_default(const char* scheme) {
  const auto names = desktop_mimeapps_names();
  const auto search_root = [&](const std::filesystem::path& root) {
    for (const auto& name : names)
      if (has_default(root / name, scheme)) return true;
    return false;
  };
  if (search_root(g_get_user_config_dir())) return true;
  for (const char* const* root = g_get_system_config_dirs(); root && *root; ++root)
    if (search_root(*root)) return true;
  if (search_root(std::filesystem::path(g_get_user_data_dir()) / "applications")) return true;
  for (const char* const* root = g_get_system_data_dirs(); root && *root; ++root)
    if (search_root(std::filesystem::path(*root) / "applications")) return true;
  return false;
}
} // namespace

GAppInfo* preferred_web_browser() {
  for (const char* scheme : {"https", "http"}) {
    if (!configured_default(scheme)) continue;
    if (auto* app = g_app_info_get_default_for_uri_scheme(scheme)) return app;
  }
  if (auto* raphael = g_desktop_app_info_new("org.midnightbsd.Raphael.desktop"))
    return G_APP_INFO(raphael);
  char* binary = g_find_program_in_path("raphael");
  if (!binary) return nullptr;
  char* quoted = g_shell_quote(binary);
  GError* error = nullptr;
  auto* app = g_app_info_create_from_commandline(quoted, "Raphael", G_APP_INFO_CREATE_NONE, &error);
  if (error) g_error_free(error);
  g_free(quoted);
  g_free(binary);
  return app;
}

std::expected<void, std::string> launch_web_browser(StartupNotification* startup) {
  auto* app = preferred_web_browser();
  if (!app) return std::unexpected("No default browser is configured and Raphael is not installed (www/raphael)");
  GError* error = nullptr;
  const bool launched = startup ? startup->launch(app, nullptr, &error) :
    g_app_info_launch(app, nullptr, nullptr, &error);
  g_object_unref(app);
  if (!launched) {
    const std::string message = error ? error->message : "Could not start the browser";
    if (error) g_error_free(error);
    return std::unexpected(message);
  }
  if (error) g_error_free(error);
  return {};
}
} // namespace midnight
