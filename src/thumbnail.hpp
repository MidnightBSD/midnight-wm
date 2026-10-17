#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace midnight {

// Reads the shared freedesktop thumbnail cache and creates 128px previews for
// image formats supported by GdkPixbuf.
class ThumbnailCache {
public:
  [[nodiscard]] std::optional<std::filesystem::path> thumbnail_for(
      const std::filesystem::path& source);

private:
  struct Entry {
    std::int64_t mtime{};
    std::uintmax_t size{};
    std::optional<std::filesystem::path> thumbnail;
  };
  std::unordered_map<std::string, Entry> entries_;
};

} // namespace midnight
