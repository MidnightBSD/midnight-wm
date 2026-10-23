#pragma once

#include <chrono>
#include <cstddef>
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
  static constexpr std::size_t max_entries = 1024;

  [[nodiscard]] std::optional<std::filesystem::path> thumbnail_for(
      const std::filesystem::path& source);
  [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
  struct Entry {
    std::int64_t mtime{};
    std::uintmax_t size{};
    std::optional<std::filesystem::path> thumbnail;
    std::chrono::steady_clock::time_point last_used{};
  };
  void remember(const std::string& key, Entry entry);
  std::unordered_map<std::string, Entry> entries_;
};

} // namespace midnight
