#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace midnight {
struct RecentFile {
  std::filesystem::path path;
  std::string title;
};

[[nodiscard]] std::vector<RecentFile> recent_files(std::size_t limit = 30);
void remember_file(const std::filesystem::path& path);
} // namespace midnight
