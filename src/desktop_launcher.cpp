#include "desktop_launcher.hpp"
#include "startup_notification.hpp"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib.h>

#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace midnight {
namespace {

using DesktopInfo = std::unique_ptr<GDesktopAppInfo, decltype(&g_object_unref)>;

std::string error_text(GError* error) {
  std::string message = error ? error->message : "unknown GIO error";
  if (error) g_error_free(error);
  return message;
}

bool trusted_entry(const std::filesystem::path& path,
                   const std::filesystem::path& desktop_directory) {
  if (path.extension() != ".desktop") return false;
  std::error_code ec;
  const auto desktop = std::filesystem::canonical(desktop_directory, ec);
  if (ec) return false;
  const auto parent = std::filesystem::canonical(path.parent_path(), ec);
  if (ec || parent != desktop) return false;

  // lstat deliberately rejects symlinks, including links to executable files.
  struct stat status {};
  return lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
         status.st_uid == geteuid() && (status.st_mode & S_IXUSR) != 0;
}

DesktopInfo read_entry(const std::filesystem::path& path) {
  if (path.extension() != ".desktop") return {nullptr, g_object_unref};
  auto* app = g_desktop_app_info_new_from_filename(path.c_str());
  if (!app) return {nullptr, g_object_unref};
  // GDesktopAppInfo can describe more than an application. File manager
  // launchers deliberately accept only application entries.
  auto* type = g_desktop_app_info_get_string(app, "Type");
  const bool application = type && std::string_view(type) == "Application";
  g_free(type);
  if (!application) {
    g_object_unref(app);
    return {nullptr, g_object_unref};
  }
  return {app, g_object_unref};
}

std::string icon_name(GIcon* icon) {
  if (!icon) return "application-x-executable";
  if (G_IS_THEMED_ICON(icon)) {
    const auto* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    if (names && names[0]) return names[0];
  }
  if (G_IS_FILE_ICON(icon)) {
    auto* file = g_file_icon_get_file(G_FILE_ICON(icon));
    auto* path = g_file_get_path(file);
    if (path) {
      std::string result(path);
      g_free(path);
      return result;
    }
  }
  return "application-x-executable";
}

} // namespace

std::expected<DesktopLauncherInfo, std::string> desktop_launcher_info(
    const std::filesystem::path& path, const std::filesystem::path& desktop_directory) {
  auto app = read_entry(path);
  if (!app) return std::unexpected("Invalid desktop application entry: " + path.string());
  const auto* name = g_app_info_get_display_name(G_APP_INFO(app.get()));
  return DesktopLauncherInfo{name ? name : path.stem().string(),
                             icon_name(g_app_info_get_icon(G_APP_INFO(app.get()))),
                             trusted_entry(path, desktop_directory)};
}

std::expected<std::vector<DesktopEntryAction>, std::string> desktop_launcher_actions(
    const std::filesystem::path& path, const std::filesystem::path& desktop_directory) {
  (void)desktop_directory;  // Trust is deliberately checked only at execution.
  auto app = read_entry(path);
  if (!app) return std::unexpected("Invalid desktop application entry: " + path.string());
  std::vector<DesktopEntryAction> result;
  auto* key_file = g_key_file_new();
  const bool loaded = g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, nullptr);
  const auto* actions = g_desktop_app_info_list_actions(app.get());
  for (std::size_t i = 0; actions && actions[i]; ++i) {
    const std::string group = std::string("Desktop Action ") + actions[i];
    auto* name = loaded ? g_key_file_get_locale_string(key_file, group.c_str(), "Name", nullptr, nullptr) : nullptr;
    auto* exec = loaded ? g_key_file_get_string(key_file, group.c_str(), "Exec", nullptr) : nullptr;
    const bool activatable = g_desktop_app_info_get_boolean(app.get(), "DBusActivatable");
    if (name && *name && (activatable || (exec && *exec))) {
      auto* icon = g_key_file_get_string(key_file, group.c_str(), "Icon", nullptr);
      result.push_back({actions[i], name, icon ? icon : ""});
      g_free(icon);
    }
    g_free(name);
    g_free(exec);
  }
  g_key_file_unref(key_file);
  return result;
}

std::vector<DesktopEntryAction> desktop_entry_actions(GAppInfo* app) {
  if (!app || !G_IS_DESKTOP_APP_INFO(app)) return {};
  const auto* filename = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app));
  if (!filename) return {};
  auto result = desktop_launcher_actions(filename, {});
  return result ? std::move(*result) : std::vector<DesktopEntryAction>{};
}

std::expected<void, std::string> launch_desktop_action(
    GAppInfo* app, const std::string& action, GAppLaunchContext* context) {
  if (!app || !G_IS_DESKTOP_APP_INFO(app))
    return std::unexpected("Application does not provide desktop actions");
  const auto* filename = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app));
  if (!filename) return std::unexpected("Application has no desktop file");
  auto current = read_entry(filename);
  if (!current) return std::unexpected("Desktop application is no longer available");
  bool offered = false;
  for (const auto& entry : desktop_entry_actions(G_APP_INFO(current.get())))
    if (entry.id == action) { offered = true; break; }
  if (!offered) return std::unexpected("Desktop action is no longer available: " + action);
  g_desktop_app_info_launch_action(current.get(), action.c_str(), context);
  return {};
}

std::expected<void, std::string> launch_desktop_entry_action(
    const std::filesystem::path& path, const std::filesystem::path& desktop_directory,
    const std::string& action, GAppLaunchContext* context) {
  if (!trusted_entry(path, desktop_directory))
    return std::unexpected("Desktop launcher is not trusted. Place it on the Desktop and mark it executable to launch it.");
  const auto actions = desktop_launcher_actions(path, desktop_directory);
  if (!actions) return std::unexpected(actions.error());
  bool offered = false;
  for (const auto& entry : *actions) if (entry.id == action) { offered = true; break; }
  if (!offered) return std::unexpected("Desktop action is no longer available: " + action);
  auto app = read_entry(path);
  if (!app) return std::unexpected("Invalid desktop application entry: " + path.string());
  g_desktop_app_info_launch_action(app.get(), action.c_str(), context);
  return {};
}

std::expected<void, std::string> launch_desktop_entry(
    const std::filesystem::path& path, const std::filesystem::path& desktop_directory,
    StartupNotification* startup) {
  if (!trusted_entry(path, desktop_directory))
    return std::unexpected("Desktop launcher is not trusted. Place it on the Desktop and mark it executable to launch it.");
  auto app = read_entry(path);
  if (!app) return std::unexpected("Invalid desktop application entry: " + path.string());
  GError* error = nullptr;
  if (!(startup ? startup->launch(G_APP_INFO(app.get()), nullptr, &error) :
        g_app_info_launch(G_APP_INFO(app.get()), nullptr, nullptr, &error)))
    return std::unexpected(error_text(error));
  return {};
}

} // namespace midnight
