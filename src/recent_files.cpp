#include "recent_files.hpp"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace midnight {
namespace {
using Bookmark = std::unique_ptr<GBookmarkFile, decltype(&g_bookmark_file_free)>;
using File = std::unique_ptr<GFile, decltype(&g_object_unref)>;

std::filesystem::path history_path() {
  return std::filesystem::path(g_get_user_data_dir()) / "recently-used.xbel";
}

Bookmark load_history(bool* readable = nullptr) {
  Bookmark bookmark(g_bookmark_file_new(), g_bookmark_file_free);
  const auto path = history_path();
  const bool exists = std::filesystem::exists(path);
  const bool loaded = g_bookmark_file_load_from_file(bookmark.get(), path.c_str(), nullptr);
  if (readable) *readable = !exists || loaded;
  return bookmark;
}
} // namespace

std::vector<RecentFile> recent_files(std::size_t limit) {
  auto bookmark = load_history();
  gsize length = 0;
  char** uris = g_bookmark_file_get_uris(bookmark.get(), &length);
  struct Entry { RecentFile file; gint64 visited; };
  std::vector<Entry> entries;
  for (gsize i = 0; i < length; ++i) {
    if (g_bookmark_file_get_is_private(bookmark.get(), uris[i], nullptr) &&
        !g_bookmark_file_has_application(bookmark.get(), uris[i], "midnight-wm", nullptr))
      continue;
    File file(g_file_new_for_uri(uris[i]), g_object_unref);
    char* path = g_file_get_path(file.get());
    if (!path) continue;
    std::filesystem::path local(path);
    g_free(path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(local, error)) continue;
    char* title = g_bookmark_file_get_title(bookmark.get(), uris[i], nullptr);
    // Owned by the bookmark file (transfer none); do not unref.
    GDateTime* date = g_bookmark_file_get_visited_date_time(bookmark.get(), uris[i], nullptr);
    entries.push_back({{local, title && *title ? title : local.filename().string()},
                       date ? g_date_time_to_unix(date) : 0});
    g_free(title);
  }
  g_strfreev(uris);
  std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
    return a.visited > b.visited;
  });
  std::vector<RecentFile> result;
  for (auto& entry : entries) {
    if (result.size() == limit) break;
    result.push_back(std::move(entry.file));
  }
  return result;
}

void remember_file(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return;
  File file(g_file_new_for_path(path.c_str()), g_object_unref);
  char* uri = g_file_get_uri(file.get());
  if (!uri) return;
  bool readable = false;
  auto bookmark = load_history(&readable);
  if (!readable) { g_free(uri); return; }
  g_bookmark_file_set_title(bookmark.get(), uri, path.filename().c_str());
  char* mime = g_content_type_guess(path.c_str(), nullptr, 0, nullptr);
  if (mime) { g_bookmark_file_set_mime_type(bookmark.get(), uri, mime); g_free(mime); }
  g_bookmark_file_add_application(bookmark.get(), uri, "midnight-wm", "midnight-wm %u");
  GDateTime* now = g_date_time_new_now_utc();
  g_bookmark_file_set_visited_date_time(bookmark.get(), uri, now);
  g_date_time_unref(now);
  g_free(uri);
  gsize length = 0;
  GError* write_error = nullptr;
  char* data = g_bookmark_file_to_data(bookmark.get(), &length, &write_error);
  if (data) {
    (void)g_mkdir_with_parents(g_get_user_data_dir(), 0700);
    File destination(g_file_new_for_path(history_path().c_str()), g_object_unref);
    (void)g_file_replace_contents(destination.get(), data, length, nullptr, FALSE,
                                  G_FILE_CREATE_PRIVATE, nullptr, nullptr, &write_error);
    g_free(data);
  }
  if (write_error) g_error_free(write_error);
}
} // namespace midnight
