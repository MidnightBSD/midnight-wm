// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "application_defaults.hpp"

#include "browser_launcher.hpp"
#include "terminal_launcher.hpp"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string_view>

namespace midnight {
namespace {
using Path = std::filesystem::path;
struct KeyFileDeleter { void operator()(GKeyFile* file) const { g_key_file_unref(file); } };
using KeyFile = std::unique_ptr<GKeyFile, KeyFileDeleter>;

const char* primary_type(AppDefaultRole role) {
  switch (role) {
    case AppDefaultRole::browser: return "x-scheme-handler/https";
    case AppDefaultRole::html: return "text/html";
    case AppDefaultRole::mail: return "x-scheme-handler/mailto";
    case AppDefaultRole::calendar: return "text/calendar";
    case AppDefaultRole::files: return "inode/directory";
    case AppDefaultRole::text: return "text/plain";
    case AppDefaultRole::images: return "image/png";
    case AppDefaultRole::pdf: return "application/pdf";
    case AppDefaultRole::terminal: return nullptr;
  }
  return nullptr;
}
std::vector<const char*> types(AppDefaultRole role) {
  switch (role) {
    case AppDefaultRole::browser: return {"x-scheme-handler/https", "x-scheme-handler/http"};
    case AppDefaultRole::images: return {"image/png", "image/jpeg"};
    default: return {primary_type(role)};
  }
}
std::string desktop_prefix() {
  const char* desktops = g_getenv("XDG_CURRENT_DESKTOP");
  if (!desktops || !*desktops) return "midnightworkspace-";
  const std::string_view first(desktops, std::strcspn(desktops, ":"));
  if (first.empty() || first.size() > 64 ||
      !std::ranges::all_of(first, [](unsigned char c) { return g_ascii_isalnum(c) || c == '-' || c == '_'; }))
    return "midnightworkspace-";
  char* lowered = g_ascii_strdown(first.data(), first.size());
  std::string prefix = lowered;
  g_free(lowered);
  return prefix + '-';
}
Path preference_path(const char* name) {
  return Path(g_get_user_config_dir()) / (desktop_prefix() + name);
}
std::optional<AppDefaultChoice> choice_from_app(GAppInfo* app) {
  if (!app || !g_app_info_get_id(app) || !g_app_info_get_display_name(app)) return std::nullopt;
  return AppDefaultChoice{g_app_info_get_id(app), g_app_info_get_display_name(app)};
}
void add_choice(std::vector<AppDefaultChoice>& choices, GAppInfo* app) {
  if (!app || !g_app_info_get_id(app) || !g_app_info_should_show(app)) return;
  if (auto choice = choice_from_app(app); choice &&
      std::ranges::none_of(choices, [&](const auto& other) { return other.id == choice->id; }))
    choices.push_back(std::move(*choice));
}
std::expected<std::string, std::string> read_optional(const Path& path) {
  char* contents = nullptr;
  gsize length = 0;
  GError* error = nullptr;
  if (!g_file_get_contents(path.c_str(), &contents, &length, &error)) {
    if (g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) { g_error_free(error); return std::string{}; }
    std::string message = error ? error->message : "Cannot read preferences";
    g_clear_error(&error);
    return std::unexpected(message);
  }
  std::string text(contents, length);
  g_free(contents);
  return text;
}
std::expected<void, std::string> write_file(const Path& path, std::string_view data) {
  if (g_mkdir_with_parents(path.parent_path().c_str(), 0700) != 0)
    return std::unexpected("Cannot create " + path.parent_path().string());
  GError* error = nullptr;
  if (!g_file_set_contents_full(path.c_str(), data.data(), static_cast<gssize>(data.size()),
      static_cast<GFileSetContentsFlags>(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE), 0600, &error)) {
    std::string message = error ? error->message : "Cannot save preferences";
    g_clear_error(&error);
    return std::unexpected(message);
  }
  return {};
}
std::vector<std::string> list_values(GKeyFile* file, const char* group, const char* key) {
  gsize count = 0;
  char** values = g_key_file_get_string_list(file, group, key, &count, nullptr);
  std::vector<std::string> result;
  for (gsize i = 0; i < count; ++i) if (values[i] && *values[i]) result.emplace_back(values[i]);
  g_strfreev(values);
  return result;
}
void set_values(GKeyFile* file, const char* group, const char* key, const std::vector<std::string>& values) {
  std::vector<const char*> pointers;
  for (const auto& value : values) pointers.push_back(value.c_str());
  g_key_file_set_string_list(file, group, key, pointers.data(), pointers.size());
}
std::expected<KeyFile, std::string> load_keyfile(const Path& path) {
  const auto contents = read_optional(path);
  if (!contents) return std::unexpected(contents.error());
  KeyFile file(g_key_file_new());
  if (!contents->empty()) {
    GError* error = nullptr;
    if (!g_key_file_load_from_data(file.get(), contents->data(), contents->size(),
        static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS), &error)) {
      std::string message = error ? error->message : "Malformed MIME preferences";
      g_clear_error(&error);
      return std::unexpected(path.string() + ": " + message);
    }
  }
  return file;
}
std::expected<void, std::string> write_keyfile(const Path& path, GKeyFile* file) {
  gsize length = 0;
  char* data = g_key_file_to_data(file, &length, nullptr);
  std::string saved(data, length);
  g_free(data);
  return write_file(path, saved);
}
std::expected<void, std::string> save_mime_defaults(const std::vector<AppDefaultSetting>& settings) {
  // Desktop-specific files may contain defaults only. Associations belong in
  // the generic user file; GIO requires them when a desktop entry lacks a type.
  const auto default_path = preference_path("mimeapps.list");
  const auto association_path = Path(g_get_user_config_dir()) / "mimeapps.list";
  auto defaults = load_keyfile(default_path);
  if (!defaults) return std::unexpected(defaults.error());
  auto associations = load_keyfile(association_path);
  if (!associations) return std::unexpected(associations.error());
  for (const auto& setting : settings) {
    if (setting.role == AppDefaultRole::terminal) continue;
    for (const char* type : types(setting.role)) {
      set_values(defaults->get(), "Default Applications", type, {setting.id});
      auto added = list_values(associations->get(), "Added Associations", type);
      if (std::ranges::find(added, setting.id) == added.end()) added.push_back(setting.id);
      set_values(associations->get(), "Added Associations", type, added);
      auto removed = list_values(associations->get(), "Removed Associations", type);
      if (std::erase(removed, setting.id)) set_values(associations->get(), "Removed Associations", type, removed);
    }
  }
  if (auto result = write_keyfile(association_path, associations->get()); !result) return result;
  return write_keyfile(default_path, defaults->get());
}
std::expected<void, std::string> save_terminal_default(const std::string& id) {
  const auto path = preference_path("xdg-terminals.list");
  const auto contents = read_optional(path);
  if (!contents) return std::unexpected(contents.error());
  std::string updated = id + '\n';
  std::string_view remaining = *contents;
  while (!remaining.empty()) {
    const auto end = remaining.find('\n');
    const auto line = remaining.substr(0, end);
    const auto start = line.find_first_not_of(" \t\r");
    const auto token_end = start == std::string_view::npos ? start : line.find_first_of(" \t\r#", start);
    const auto token = start == std::string_view::npos ? std::string_view{} : line.substr(start, token_end - start);
    if (token != id) { updated.append(line); updated += '\n'; }
    if (end == std::string_view::npos) break;
    remaining.remove_prefix(end + 1);
  }
  return write_file(path, updated);
}
} // namespace

const char* app_default_label(AppDefaultRole role) {
  switch (role) {
    case AppDefaultRole::browser: return "Web browser";
    case AppDefaultRole::html: return "HTML files";
    case AppDefaultRole::mail: return "Email links";
    case AppDefaultRole::calendar: return "Calendar files";
    case AppDefaultRole::terminal: return "Terminal";
    case AppDefaultRole::files: return "Folders";
    case AppDefaultRole::text: return "Text files";
    case AppDefaultRole::images: return "Images (PNG/JPEG)";
    case AppDefaultRole::pdf: return "PDF files";
  }
  return "Application";
}
std::optional<AppDefaultChoice> current_app_default(AppDefaultRole role) {
  GAppInfo* app = role == AppDefaultRole::terminal ? preferred_terminal() :
                  role == AppDefaultRole::browser ? preferred_web_browser() :
                  g_app_info_get_default_for_type(primary_type(role), false);
  auto result = choice_from_app(app);
  if (app) g_object_unref(app);
  return result;
}
std::vector<AppDefaultChoice> app_default_choices(AppDefaultRole role) {
  std::vector<AppDefaultChoice> choices;
  if (role == AppDefaultRole::terminal) {
    GList* apps = g_app_info_get_all();
    for (GList* item = apps; item; item = item->next) {
      auto* app = G_APP_INFO(item->data);
      if (!G_IS_DESKTOP_APP_INFO(app)) continue;
      const char* categories = g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(app));
      if (categories && std::string_view(categories).find("TerminalEmulator") != std::string_view::npos)
        add_choice(choices, app);
    }
    g_list_free_full(apps, g_object_unref);
  } else for (const char* type : types(role)) {
    // Recommended handlers claim this type directly; the broader GIO list
    // also includes generic text editors through MIME parent types.
    GList* apps = g_app_info_get_recommended_for_type(type);
    for (GList* item = apps; item; item = item->next) add_choice(choices, G_APP_INFO(item->data));
    g_list_free_full(apps, g_object_unref);
  }
  if (role == AppDefaultRole::files) {
    // A freshly installed desktop entry may not be in mimeinfo.cache yet.
    if (auto* app = g_desktop_app_info_new("org.midnightbsd.Workspace.Files.desktop")) {
      add_choice(choices, G_APP_INFO(app));
      g_object_unref(app);
    }
  }
  if (auto current = current_app_default(role);
      current && std::ranges::none_of(choices, [&](const auto& value) { return value.id == current->id; }))
    choices.push_back(std::move(*current));
  std::ranges::sort(choices, [](const auto& left, const auto& right) {
    return g_utf8_collate(left.name.c_str(), right.name.c_str()) < 0;
  });
  return choices;
}
std::expected<void, std::string> save_app_defaults(const std::vector<AppDefaultSetting>& settings) {
  if (settings.empty()) return {};
  for (const auto& setting : settings) {
    GDesktopAppInfo* app = g_desktop_app_info_new(setting.id.c_str());
    if (!app) return std::unexpected("Application is no longer installed: " + setting.id);
    g_object_unref(app);
  }
  if (std::ranges::any_of(settings, [](const auto& value) { return value.role != AppDefaultRole::terminal; })) {
    if (auto result = save_mime_defaults(settings); !result) return result;
  }
  for (const auto& setting : settings)
    if (setting.role == AppDefaultRole::terminal)
      if (auto result = save_terminal_default(setting.id); !result) return result;
  return {};
}
} // namespace midnight
