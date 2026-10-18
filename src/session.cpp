// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "session.hpp"
#include "private_state.hpp"

#include <glib.h>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

namespace midnight {
namespace {
constexpr std::size_t snapshot_limit = 1024 * 1024;
constexpr std::size_t folder_limit = 128;
using KeyFile = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;

bool valid(const SessionSnapshot& snapshot) {
  if (snapshot.workspace >= 4 || snapshot.folders.size() > folder_limit) return false;
  for (const auto& folder : snapshot.folders) {
    const auto& r = folder.geometry;
    if (folder.workspace >= 4 || folder.path.empty() || folder.path.size() > 4096 ||
        folder.path.find('\0') != std::string::npos ||
        !g_utf8_validate(folder.path.data(), folder.path.size(), nullptr) ||
        (folder.path != "trash:///" && !std::filesystem::path(folder.path).is_absolute()) ||
        r.x < -32768 || r.x > 32767 || r.y < -32768 || r.y > 32767 ||
        r.width < 1 || r.width > 32767 || r.height < 1 || r.height > 32767) return false;
  }
  return true;
}

} // namespace

SessionGeometry recover_session_geometry(SessionGeometry saved, const std::vector<SessionGeometry>& areas) {
  if (areas.empty()) return saved;
  const SessionGeometry* best = nullptr;
  auto distance = std::numeric_limits<std::int64_t>::max();
  for (const auto& area : areas) {
    if (area.width < 1 || area.height < 1) continue;
    const auto cx = std::int64_t(saved.x) + saved.width / 2;
    const auto cy = std::int64_t(saved.y) + saved.height / 2;
    const auto dx = cx - std::clamp(cx, std::int64_t(area.x), std::int64_t(area.x) + area.width - 1);
    const auto dy = cy - std::clamp(cy, std::int64_t(area.y), std::int64_t(area.y) + area.height - 1);
    if (!best || dx * dx + dy * dy < distance) { best = &area; distance = dx * dx + dy * dy; }
  }
  if (!best) return saved;
  saved.width = std::clamp(saved.width, std::min(250, best->width), best->width);
  saved.height = std::clamp(saved.height, std::min(180, best->height), best->height);
  saved.x = std::clamp(saved.x, best->x, best->x + best->width - saved.width);
  saved.y = std::clamp(saved.y, best->y, best->y + best->height - saved.height);
  return saved;
}

SessionStore::SessionStore(std::filesystem::path path) : path_(std::move(path)) {}
std::filesystem::path SessionStore::default_path() {
  const char* state = std::getenv("XDG_STATE_HOME");
  const auto base = state && std::filesystem::path(state).is_absolute()
      ? std::filesystem::path(state) : std::filesystem::path(g_get_home_dir()) / ".local/state";
  return base / "midnight-wm/session.ini";
}

std::expected<std::optional<SessionSnapshot>, std::string> SessionStore::load() const {
  auto loaded = read_private_state(path_, snapshot_limit);
  if (!loaded) return std::unexpected(loaded.error());
  if (!*loaded) return std::nullopt;
  const auto& contents = **loaded;
  KeyFile keys(g_key_file_new(), g_key_file_unref);
  if (contents.find('\0') != std::string::npos ||
      !g_key_file_load_from_data(keys.get(), contents.data(), contents.size(), G_KEY_FILE_NONE, nullptr))
    return std::unexpected("Malformed session snapshot");
  bool parsed = true;
  auto integer = [&](const char* group, const char* name) {
    GError* error = nullptr;
    const auto value = g_key_file_get_integer(keys.get(), group, name, &error);
    if (error) { parsed = false; g_error_free(error); }
    return value;
  };
  auto boolean = [&](const char* group, const char* name) {
    GError* error = nullptr;
    const bool value = g_key_file_get_boolean(keys.get(), group, name, &error);
    if (error) { parsed = false; g_error_free(error); }
    return value;
  };
  if (integer("Session", "version") != 1) return std::unexpected("Unsupported session snapshot version");
  SessionSnapshot snapshot;
  snapshot.workspace = static_cast<std::uint32_t>(integer("Session", "workspace"));
  const int count = integer("Session", "folders");
  if (!parsed || count < 0 || count > static_cast<int>(folder_limit))
    return std::unexpected("Invalid session folder count");
  for (int i = 0; i < count; ++i) {
    const auto group = "Folder " + std::to_string(i);
    SessionFolder folder;
    char* path = g_key_file_get_string(keys.get(), group.c_str(), "path", nullptr);
    folder.path = path ? path : "";
    g_free(path);
    folder.geometry = {integer(group.c_str(), "x"), integer(group.c_str(), "y"),
                       integer(group.c_str(), "width"), integer(group.c_str(), "height")};
    folder.workspace = static_cast<std::uint32_t>(integer(group.c_str(), "workspace"));
    folder.maximized = boolean(group.c_str(), "maximized");
    folder.shaded = boolean(group.c_str(), "shaded");
    snapshot.folders.push_back(std::move(folder));
  }
  if (!parsed || !valid(snapshot)) return std::unexpected("Invalid session snapshot values");
  return snapshot;
}

std::expected<void, std::string> SessionStore::save(const SessionSnapshot& snapshot) const {
  if (!valid(snapshot)) return std::unexpected("Invalid session snapshot values");
  KeyFile keys(g_key_file_new(), g_key_file_unref);
  auto integer = [&](const char* group, const char* name, int value) { g_key_file_set_integer(keys.get(), group, name, value); };
  integer("Session", "version", 1);
  integer("Session", "workspace", static_cast<int>(snapshot.workspace));
  integer("Session", "folders", static_cast<int>(snapshot.folders.size()));
  for (std::size_t i = 0; i < snapshot.folders.size(); ++i) {
    const auto group = "Folder " + std::to_string(i);
    const auto& folder = snapshot.folders[i];
    g_key_file_set_string(keys.get(), group.c_str(), "path", folder.path.c_str());
    integer(group.c_str(), "x", folder.geometry.x);
    integer(group.c_str(), "y", folder.geometry.y);
    integer(group.c_str(), "width", folder.geometry.width);
    integer(group.c_str(), "height", folder.geometry.height);
    integer(group.c_str(), "workspace", static_cast<int>(folder.workspace));
    g_key_file_set_boolean(keys.get(), group.c_str(), "maximized", folder.maximized);
    g_key_file_set_boolean(keys.get(), group.c_str(), "shaded", folder.shaded);
  }
  gsize length{};
  std::unique_ptr<gchar, decltype(&g_free)> contents(g_key_file_to_data(keys.get(), &length, nullptr), g_free);
  if (length > snapshot_limit) return std::unexpected("Session snapshot is too large");
  return write_private_state(path_, std::string_view(contents.get(), length));
}

std::expected<void, std::string> SessionStore::clear() const {
  return clear_private_state(path_);
}
} // namespace midnight
