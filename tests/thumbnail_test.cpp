#include "thumbnail.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>

namespace fs = std::filesystem;

int main() {
  char* uuid = g_uuid_string_random();
  const auto root = fs::temp_directory_path() / ("midnight-thumbnail-" + std::string(uuid));
  g_free(uuid);
  fs::create_directories(root);
  const auto cache = root / "cache";
  g_setenv("XDG_CACHE_HOME", cache.c_str(), TRUE);
  const auto source = root / "sample.png";
  auto* image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 32, 16);
  gdk_pixbuf_fill(image, 0x6a3e9dff);
  GError* error = nullptr;
  const bool saved = gdk_pixbuf_save(image, source.c_str(), "png", &error, nullptr);
  g_object_unref(image);
  if (error) g_error_free(error);
  if (!saved) { std::cerr << "source image save failed\n"; return 1; }

  midnight::ThumbnailCache thumbnails;
  const auto first = thumbnails.thumbnail_for(source);
  if (!first || !fs::exists(*first) || first->parent_path() != cache / "thumbnails/normal") {
    std::cerr << "standard thumbnail was not created\n";
    return 1;
  }
  auto* preview = gdk_pixbuf_new_from_file(first->c_str(), nullptr);
  const bool metadata = preview &&
    gdk_pixbuf_get_option(preview, "tEXt::Thumb::URI") &&
    gdk_pixbuf_get_option(preview, "tEXt::Thumb::MTime");
  if (preview) g_object_unref(preview);
  if (!metadata) { std::cerr << "thumbnail metadata missing\n"; return 1; }

  const auto second = thumbnails.thumbnail_for(source);
  if (second != first) { std::cerr << "thumbnail cache miss\n"; return 1; }
  struct stat details{};
  if (stat(source.c_str(), &details) != 0) return 1;
  const struct timespec times[2]{{details.st_atime, 0}, {details.st_mtime + 2, 0}};
  if (utimensat(AT_FDCWD, source.c_str(), times, 0) != 0) return 1;
  const auto refreshed = thumbnails.thumbnail_for(source);
  if (refreshed != first) { std::cerr << "thumbnail was not refreshed\n"; return 1; }
  preview = gdk_pixbuf_new_from_file(refreshed->c_str(), nullptr);
  const char* refreshed_mtime = preview ? gdk_pixbuf_get_option(preview, "tEXt::Thumb::MTime") : nullptr;
  const bool fresh_metadata = refreshed_mtime &&
    std::string(refreshed_mtime) == std::to_string(details.st_mtime + 2);
  if (preview) g_object_unref(preview);
  if (!fresh_metadata) { std::cerr << "stale thumbnail metadata\n"; return 1; }
  std::error_code ignored;
  fs::remove_all(root, ignored);
  return 0;
}
