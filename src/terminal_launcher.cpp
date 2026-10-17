#include "terminal_launcher.hpp"

#include "startup_notification.hpp"

#include <gio/gdesktopappinfo.h>
#include <glib.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace midnight {
namespace {
std::vector<std::string> preference_names() {
  std::vector<std::string> names;
  if (const char* desktops = g_getenv("XDG_CURRENT_DESKTOP")) {
    char** parts = g_strsplit(desktops, ":", -1);
    for (char** part = parts; part && *part; ++part) {
      if (!**part) continue;
      char* lower = g_ascii_strdown(*part, -1);
      names.emplace_back(std::string(lower) + "-xdg-terminals.list");
      g_free(lower);
    }
    g_strfreev(parts);
  }
  names.emplace_back("xdg-terminals.list");
  return names;
}

GAppInfo* terminal_from_list(const std::filesystem::path& path) {
  std::ifstream input(path);
  for (std::string line; std::getline(input, line);) {
    const auto start = line.find_first_not_of(" \t\r");
    if (start == std::string::npos || line[start] == '#') continue;
    const auto end = line.find_first_of(" \t\r#", start);
    const auto id = line.substr(start, end - start);
    if (!id.ends_with(".desktop")) continue;
    if (auto* app = g_desktop_app_info_new(id.c_str())) return G_APP_INFO(app);
  }
  return nullptr;
}

GAppInfo* configured_terminal() {
  const auto names = preference_names();
  const auto search_root = [&](const std::filesystem::path& root) -> GAppInfo* {
    for (const auto& name : names)
      if (auto* app = terminal_from_list(root / name)) return app;
    return nullptr;
  };
  if (auto* app = search_root(g_get_user_config_dir())) return app;
  for (const char* const* root = g_get_system_config_dirs(); root && *root; ++root)
    if (auto* app = search_root(*root)) return app;
  for (const char* const* root = g_get_system_data_dirs(); root && *root; ++root)
    if (auto* app = search_root(std::filesystem::path(*root) / "xdg-terminal-exec")) return app;
  return nullptr;
}

GAppInfo* command_app(const char* command, const char* name) {
  int count = 0;
  char** args = nullptr;
  if (!g_shell_parse_argv(command, &count, &args, nullptr)) return nullptr;
  char* binary = count ? g_find_program_in_path(args[0]) : nullptr;
  g_strfreev(args);
  if (!binary) return nullptr;
  g_free(binary);
  GError* error = nullptr;
  auto* app = g_app_info_create_from_commandline(command, name, G_APP_INFO_CREATE_NONE, &error);
  if (error) g_error_free(error);
  return app;
}
} // namespace

GAppInfo* preferred_terminal() {
  if (auto* app = configured_terminal()) return app;
  if (const char* command = g_getenv("TERMINAL"); command && *command)
    if (auto* app = command_app(command, "Terminal")) return app;
  if (auto* xterm = g_desktop_app_info_new("xterm.desktop")) return G_APP_INFO(xterm);
  return command_app("xterm", "XTerm");
}

std::expected<void, std::string> launch_terminal(StartupNotification* startup) {
  auto* app = preferred_terminal();
  if (!app) return std::unexpected("No terminal is configured and xterm is not installed (x11/xterm)");
  GError* error = nullptr;
  const bool launched = startup ? startup->launch(app, nullptr, &error) :
    g_app_info_launch(app, nullptr, nullptr, &error);
  g_object_unref(app);
  if (!launched) {
    const std::string message = error ? error->message : "Could not start the terminal";
    if (error) g_error_free(error);
    return std::unexpected(message);
  }
  if (error) g_error_free(error);
  return {};
}
} // namespace midnight
