#include "thumbnail.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace midnight {
namespace {
using Pixbuf = std::unique_ptr<GdkPixbuf, decltype(&g_object_unref)>;
using File = std::unique_ptr<GFile, decltype(&g_object_unref)>;

std::string canonical_uri(const std::filesystem::path& source) {
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(source, error);
  if (error) return {};
  File file(g_file_new_for_path(canonical.c_str()), g_object_unref);
  char* uri = g_file_get_uri(file.get());
  std::string result = uri ? uri : "";
  g_free(uri);
  return result;
}

bool valid_thumbnail(const std::filesystem::path& path, const std::string& uri,
                     std::int64_t mtime, std::uintmax_t size) {
  Pixbuf image(gdk_pixbuf_new_from_file(path.c_str(), nullptr), g_object_unref);
  if (!image) return false;
  const char* stored_uri = gdk_pixbuf_get_option(image.get(), "tEXt::Thumb::URI");
  const char* stored_mtime = gdk_pixbuf_get_option(image.get(), "tEXt::Thumb::MTime");
  const char* stored_size = gdk_pixbuf_get_option(image.get(), "tEXt::Thumb::Size");
  return stored_uri && uri == stored_uri && stored_mtime &&
    std::to_string(mtime) == stored_mtime &&
    (!stored_size || std::to_string(size) == stored_size);
}

bool save_thumbnail(GdkPixbuf* image, const std::filesystem::path& destination,
                    const std::string& uri, std::int64_t mtime, std::uintmax_t size) {
  const auto directory = destination.parent_path();
  if (g_mkdir_with_parents(directory.c_str(), 0700) != 0) return false;
  std::string temporary = (directory / ".midnight-wm-XXXXXX").string();
  std::vector<char> name(temporary.begin(), temporary.end());
  name.push_back('\0');
  const int fd = mkstemp(name.data());
  if (fd < 0) return false;
  close(fd);
  const auto mtime_text = std::to_string(mtime);
  const auto size_text = std::to_string(size);
  GError* error = nullptr;
  const bool saved = gdk_pixbuf_save(image, name.data(), "png", &error,
                                     "tEXt::Thumb::URI", uri.c_str(),
                                     "tEXt::Thumb::MTime", mtime_text.c_str(),
                                     "tEXt::Thumb::Size", size_text.c_str(),
                                     "tEXt::Software", "midnight-wm", nullptr);
  if (error) g_error_free(error);
  if (!saved || g_chmod(name.data(), 0600) != 0 ||
      g_rename(name.data(), destination.c_str()) != 0) {
    g_unlink(name.data());
    return false;
  }
  return true;
}
} // namespace

void ThumbnailCache::remember(const std::string& key, Entry entry) {
  // Bound positive and negative lookups together. Evict only metadata; the
  // shared on-disk thumbnail cache and shell-owned image surfaces stay valid.
  if (entries_.size() >= max_entries && !entries_.contains(key)) {
    const auto oldest = std::ranges::min_element(entries_, {},
        [](const auto& item) { return item.second.last_used; });
    entries_.erase(oldest);
  }
  entry.last_used = std::chrono::steady_clock::now();
  entries_.insert_or_assign(key, std::move(entry));
}

std::optional<std::filesystem::path> ThumbnailCache::thumbnail_for(
    const std::filesystem::path& source) {
  const auto key = source.string();
  struct stat details{};
  if (stat(source.c_str(), &details) != 0 || !S_ISREG(details.st_mode)) {
    entries_.erase(key);
    return std::nullopt;
  }
  const auto mtime = static_cast<std::int64_t>(details.st_mtime);
  const auto size = static_cast<std::uintmax_t>(details.st_size);
  if (auto previous = entries_.find(key); previous != entries_.end() &&
      previous->second.mtime == mtime && previous->second.size == size) {
    previous->second.last_used = std::chrono::steady_clock::now();
    return previous->second.thumbnail;
  }

  Entry entry{mtime, size, std::nullopt};
  const auto uri = canonical_uri(source);
  if (uri.empty()) { remember(key, std::move(entry)); return std::nullopt; }
  char* digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri.c_str(), -1);
  const auto thumbnail = std::filesystem::path(g_get_user_cache_dir()) / "thumbnails" /
                         "normal" / (std::string(digest) + ".png");
  g_free(digest);
  if (valid_thumbnail(thumbnail, uri, mtime, size)) {
    entry.thumbnail = thumbnail;
  } else if (size <= 64u * 1024u * 1024u && gdk_pixbuf_get_file_info(source.c_str(), nullptr, nullptr)) {
    GError* error = nullptr;
    Pixbuf image(gdk_pixbuf_new_from_file_at_scale(source.c_str(), 128, 128, TRUE, &error),
                 g_object_unref);
    if (error) g_error_free(error);
    if (image && save_thumbnail(image.get(), thumbnail, uri, mtime, size))
      entry.thumbnail = thumbnail;
  }
  remember(key, entry);
  return entry.thumbnail;
}

} // namespace midnight
