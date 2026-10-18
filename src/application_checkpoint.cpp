// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "application_session.hpp"
#include "private_state.hpp"

#include <glib.h>
#include <memory>
#include <set>
#include <utility>

namespace midnight {
namespace {
constexpr std::size_t file_limit = 1024 * 1024;
constexpr int application_limit = 128;
constexpr int window_limit = 512;
using KeyFile = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;
std::string encode(const std::string& text) {
  char* encoded = g_base64_encode(reinterpret_cast<const guchar*>(text.data()), text.size());
  std::string result(encoded);
  g_free(encoded);
  return result;
}
bool valid(const ApplicationCheckpoint& checkpoint) {
  if (checkpoint.applications.size() > application_limit || checkpoint.windows.size() > window_limit) return false;
  std::set<std::string> ids;
  for (const auto& application : checkpoint.applications) {
    if (!validate_application_restart(application) || application.style == RestartStyle::never ||
        !ids.insert(application.client_id).second) return false;
  }
  std::set<std::pair<std::string, std::string>> identities;
  for (const auto& window : checkpoint.windows) {
    if (!validate_application_window(window) || !ids.contains(window.client_id) ||
        !identities.emplace(window.client_id, window.role).second) return false;
  }
  return true;
}
} // namespace

ApplicationSessionStore::ApplicationSessionStore(std::filesystem::path path) : path_(std::move(path)) {}
std::filesystem::path ApplicationSessionStore::default_path() {
  return SessionStore::default_path().parent_path() / "applications.ini";
}
std::expected<std::optional<ApplicationCheckpoint>, std::string> ApplicationSessionStore::load() const {
  auto loaded = read_private_state(path_, file_limit);
  if (!loaded) return std::unexpected(loaded.error());
  if (!*loaded) return std::nullopt;
  const auto& contents = **loaded;
  KeyFile keys(g_key_file_new(), g_key_file_unref);
  if (contents.find('\0') != std::string::npos ||
      !g_key_file_load_from_data(keys.get(), contents.data(), contents.size(), G_KEY_FILE_NONE, nullptr))
    return std::unexpected("Malformed application checkpoint");
  bool parsed = true;
  const auto integer = [&](const char* group, const char* name) {
    GError* error = nullptr;
    const int value = g_key_file_get_integer(keys.get(), group, name, &error);
    if (error) { parsed = false; g_error_free(error); }
    return value;
  };
  // Base64 preserves the bytes and boundaries of XSMP arguments, including
  // non-UTF-8 filenames. Require canonical encoding rather than silently
  // accepting GLib's permissive decoder output for malformed input.
  const auto text = [&](const char* group, const char* name) {
    char* value = g_key_file_get_string(keys.get(), group, name, nullptr);
    if (!value) { parsed = false; return std::string{}; }
    gsize length{};
    guchar* decoded = g_base64_decode(value, &length);
    std::string result;
    if (length) result.assign(reinterpret_cast<const char*>(decoded), length);
    if (encode(result) != value) parsed = false;
    g_free(decoded);
    g_free(value);
    return result;
  };
  const int version = integer("Applications", "version");
  if (version != 1 && version != 2) return std::unexpected("Unsupported application checkpoint version");
  const int count = integer("Applications", "count");
  if (!parsed || count < 0 || count > application_limit) return std::unexpected("Invalid application count");
  ApplicationCheckpoint checkpoint;
  for (int i = 0; i < count; ++i) {
    const auto group = "Application " + std::to_string(i);
    ApplicationRestart application;
    application.client_id = text(group.c_str(), "client_id");
    application.program = text(group.c_str(), "program");
    application.directory = text(group.c_str(), "directory");
    const int style = integer(group.c_str(), "style");
    if (style < 0 || style > static_cast<int>(RestartStyle::never)) parsed = false;
    application.style = static_cast<RestartStyle>(style);
    const int argc = integer(group.c_str(), "argc");
    if (!parsed || argc < 1 || argc > 128) return std::unexpected("Invalid application restart fields");
    for (int arg = 0; arg < argc; ++arg)
      application.arguments.push_back(text(group.c_str(), ("arg" + std::to_string(arg)).c_str()));
    checkpoint.applications.push_back(std::move(application));
  }
  if (version == 2) {
    const int windows = integer("Applications", "windows");
    if (!parsed || windows < 0 || windows > window_limit) return std::unexpected("Invalid application window count");
    for (int i = 0; i < windows; ++i) {
      const auto group = "Window " + std::to_string(i);
      ApplicationWindow window;
      window.client_id = text(group.c_str(), "client_id");
      window.role = text(group.c_str(), "role");
      window.geometry = {integer(group.c_str(), "x"), integer(group.c_str(), "y"),
                         integer(group.c_str(), "width"), integer(group.c_str(), "height")};
      window.workspace = static_cast<std::uint32_t>(integer(group.c_str(), "workspace"));
      const int states = integer(group.c_str(), "states");
      if (states < 0 || states > 15) parsed = false;
      window.maximized_horz = states & 1;
      window.maximized_vert = states & 2;
      window.fullscreen = states & 4;
      window.shaded = states & 8;
      checkpoint.windows.push_back(std::move(window));
    }
  }
  if (!parsed || !valid(checkpoint)) return std::unexpected("Invalid application checkpoint values");
  return checkpoint;
}

std::expected<void, std::string> ApplicationSessionStore::save(const ApplicationCheckpoint& checkpoint) const {
  if (!valid(checkpoint)) return std::unexpected("Invalid application checkpoint values");
  if (auto previous = load(); !previous) return std::unexpected(previous.error());
  KeyFile keys(g_key_file_new(), g_key_file_unref);
  g_key_file_set_integer(keys.get(), "Applications", "version", 2);
  g_key_file_set_integer(keys.get(), "Applications", "windows", static_cast<int>(checkpoint.windows.size()));
  g_key_file_set_integer(keys.get(), "Applications", "count", static_cast<int>(checkpoint.applications.size()));
  for (std::size_t i = 0; i < checkpoint.applications.size(); ++i) {
    const auto group = "Application " + std::to_string(i);
    const auto& application = checkpoint.applications[i];
    const auto text = [&](const char* name, const std::string& value) {
      const auto encoded = encode(value);
      g_key_file_set_string(keys.get(), group.c_str(), name, encoded.c_str());
    };
    text("client_id", application.client_id);
    text("program", application.program);
    text("directory", application.directory);
    g_key_file_set_integer(keys.get(), group.c_str(), "style", static_cast<int>(application.style));
    g_key_file_set_integer(keys.get(), group.c_str(), "argc", static_cast<int>(application.arguments.size()));
    for (std::size_t arg = 0; arg < application.arguments.size(); ++arg)
      text(("arg" + std::to_string(arg)).c_str(), application.arguments[arg]);
  }
  for (std::size_t i = 0; i < checkpoint.windows.size(); ++i) {
    const auto group = "Window " + std::to_string(i);
    const auto& window = checkpoint.windows[i];
    const auto id = encode(window.client_id), role = encode(window.role);
    g_key_file_set_string(keys.get(), group.c_str(), "client_id", id.c_str());
    g_key_file_set_string(keys.get(), group.c_str(), "role", role.c_str());
    const auto integer = [&](const char* name, int value) {
      g_key_file_set_integer(keys.get(), group.c_str(), name, value);
    };
    integer("x", window.geometry.x); integer("y", window.geometry.y);
    integer("width", window.geometry.width); integer("height", window.geometry.height);
    integer("workspace", static_cast<int>(window.workspace));
    integer("states", (window.maximized_horz ? 1 : 0) | (window.maximized_vert ? 2 : 0) |
                      (window.fullscreen ? 4 : 0) | (window.shaded ? 8 : 0));
  }
  gsize length{};
  std::unique_ptr<gchar, decltype(&g_free)> contents(g_key_file_to_data(keys.get(), &length, nullptr), g_free);
  if (length > file_limit) return std::unexpected("Application checkpoint is too large");
  return write_private_state(path_, std::string_view(contents.get(), length));
}
std::expected<void, std::string> ApplicationSessionStore::clear() const { return clear_private_state(path_); }
} // namespace midnight
