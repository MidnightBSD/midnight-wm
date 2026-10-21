// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include "startup_applications.hpp"

#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <memory>
#include <set>

namespace midnight {
namespace {
using KeyFile = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;
using DesktopInfo = std::unique_ptr<GDesktopAppInfo, decltype(&g_object_unref)>;

std::string error_text(GError* error, const char* fallback) {
  std::string message = error ? error->message : fallback;
  g_clear_error(&error);
  return message;
}

bool safe_regular_file(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::symlink_status(path, error).type() == std::filesystem::file_type::regular;
}

std::expected<KeyFile, std::string> load(const std::filesystem::path& path) {
  if (!safe_regular_file(path)) return std::unexpected("Autostart entry is not a regular file: " + path.string());
  KeyFile file(g_key_file_new(), g_key_file_unref);
  GError* error = nullptr;
  if (!g_key_file_load_from_file(file.get(), path.c_str(),
          static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS), &error))
    return std::unexpected(error_text(error, "Could not read autostart entry"));
  if (!g_key_file_has_group(file.get(), "Desktop Entry"))
    return std::unexpected("Autostart entry has no Desktop Entry group: " + path.string());
  return file;
}

std::expected<void, std::string> write(const std::filesystem::path& path, GKeyFile* file) {
  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(path, status_error);
  if (!status_error && status.type() != std::filesystem::file_type::not_found)
    return std::unexpected("Autostart entry already exists: " + path.string());
  if (status_error && status_error != std::errc::no_such_file_or_directory)
    return std::unexpected(status_error.message());
  if (g_mkdir_with_parents(path.parent_path().c_str(), 0700) != 0)
    return std::unexpected("Could not create autostart directory");
  gsize length = 0;
  auto* data = g_key_file_to_data(file, &length, nullptr);
  GError* error = nullptr;
  const bool saved = g_file_set_contents_full(path.c_str(), data, static_cast<gssize>(length),
      static_cast<GFileSetContentsFlags>(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE),
      0600, &error);
  g_free(data);
  if (!saved) return std::unexpected(error_text(error, "Could not save autostart entry"));
  return {};
}

std::expected<void, std::string> replace(const std::filesystem::path& path, GKeyFile* file) {
  if (!safe_regular_file(path)) return std::unexpected("Autostart entry changed or is not a regular file");
  gsize length = 0;
  auto* data = g_key_file_to_data(file, &length, nullptr);
  GError* error = nullptr;
  const bool saved = g_file_set_contents_full(path.c_str(), data, static_cast<gssize>(length),
      static_cast<GFileSetContentsFlags>(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE),
      0600, &error);
  g_free(data);
  if (!saved) return std::unexpected(error_text(error, "Could not save autostart entry"));
  return {};
}

bool available_on_this_desktop(GDesktopAppInfo* info) {
  const char* desktop = g_getenv("XDG_CURRENT_DESKTOP");
  if (!g_desktop_app_info_get_show_in(info, desktop && *desktop ? nullptr : "MidnightWorkspace"))
    return false;
  std::unique_ptr<char, decltype(&g_free)> try_exec(
      g_desktop_app_info_get_string(info, "TryExec"), g_free);
  if (try_exec && *try_exec) {
    std::unique_ptr<char, decltype(&g_free)> executable(g_find_program_in_path(try_exec.get()), g_free);
    if (!executable) return false;
  }
  return true;
}
} // namespace

std::vector<StartupApplication> startup_applications(
    const std::vector<std::filesystem::path>& config_roots) {
  std::set<std::string> seen;
  std::vector<StartupApplication> result;
  for (std::size_t root_index = 0; root_index < config_roots.size(); ++root_index) {
    std::error_code error;
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator it(config_roots[root_index] / "autostart", error), end;
         !error && it != end; it.increment(error)) {
      if (it->path().extension() == ".desktop" && it->is_regular_file(error))
        candidates.push_back(it->path());
    }
    std::ranges::sort(candidates);
    for (const auto& path : candidates) {
      const auto id = path.filename().string();
      if (!seen.insert(id).second) continue;
      auto file = load(path);
      if (!file) continue;
      DesktopInfo info(g_desktop_app_info_new_from_keyfile(file->get()), g_object_unref);
      if (!info) continue;
      const char* name = g_app_info_get_display_name(G_APP_INFO(info.get()));
      const char* command = g_app_info_get_commandline(G_APP_INFO(info.get()));
      const bool hidden = g_key_file_get_boolean(file->get(), "Desktop Entry", "Hidden", nullptr);
      const bool available = available_on_this_desktop(info.get());
      result.push_back({id, name ? name : id, command ? command : "", path,
                        !hidden && available, available, root_index == 0});
    }
  }
  std::ranges::sort(result, [](const auto& left, const auto& right) {
    return left.name == right.name ? left.id < right.id : left.name < right.name;
  });
  return result;
}

std::expected<void, std::string> set_startup_application_enabled(
    const StartupApplication& application, bool enabled,
    const std::filesystem::path& user_config_root) {
  if (!application.available) return std::unexpected("This entry does not apply to Midnight Workspace");
  const auto target = user_config_root / "autostart" / application.id;
  if (application.path.filename() != application.id ||
      (application.user_entry && application.path != target))
    return std::unexpected("Invalid autostart entry path");
  auto file = load(application.path);
  if (!file) return std::unexpected(file.error());
  g_key_file_set_boolean(file->get(), "Desktop Entry", "Hidden", !enabled);
  if (application.user_entry) return replace(target, file->get());
  return write(target, file->get());
}

std::expected<std::string, std::string> add_startup_application(
    const std::string& name, const std::string& command,
    const std::filesystem::path& user_config_root) {
  if (name.empty() || command.empty() || name.size() > 200 || command.size() > 2048 ||
      name.find_first_of("\r\n") != std::string::npos || command.find_first_of("\r\n") != std::string::npos)
    return std::unexpected("Enter a name and command without line breaks");
  KeyFile file(g_key_file_new(), g_key_file_unref);
  g_key_file_set_string(file.get(), "Desktop Entry", "Type", "Application");
  g_key_file_set_string(file.get(), "Desktop Entry", "Name", name.c_str());
  g_key_file_set_string(file.get(), "Desktop Entry", "Exec", command.c_str());
  DesktopInfo info(g_desktop_app_info_new_from_keyfile(file.get()), g_object_unref);
  if (!info || !g_app_info_get_executable(G_APP_INFO(info.get())))
    return std::unexpected("Enter a valid desktop entry command");
  auto* uuid = g_uuid_string_random();
  const std::string id = "midnight-" + std::string(uuid) + ".desktop";
  g_free(uuid);
  if (auto saved = write(user_config_root / "autostart" / id, file.get()); !saved)
    return std::unexpected(saved.error());
  return id;
}

std::expected<void, std::string> remove_startup_application(
    const StartupApplication& application, const std::filesystem::path& user_config_root) {
  const auto target = user_config_root / "autostart" / application.id;
  if (!application.user_entry || application.path != target || !safe_regular_file(target))
    return std::unexpected("Only user autostart entries can be removed");
  if (g_unlink(target.c_str()) != 0) return std::unexpected("Could not remove autostart entry");
  return {};
}

} // namespace midnight
