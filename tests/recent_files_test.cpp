#include "recent_files.hpp"

#include <glib.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main() {
  char pattern[] = "/tmp/midnight-recent-XXXXXX";
  char* directory = mkdtemp(pattern);
  if (!directory) return 1;
  const auto root = std::filesystem::path(directory);
  const auto data = root / "data";
  g_setenv("XDG_DATA_HOME", data.c_str(), TRUE);
  const auto file = root / "example.txt";
  std::ofstream(file) << "recent file\n";
  midnight::remember_file(file);
  const auto history = data / "recently-used.xbel";
  if (!std::filesystem::is_regular_file(history)) return 2;
  const auto entries = midnight::recent_files();
  if (entries.size() != 1 || entries[0].path != file || entries[0].title != "example.txt") return 3;
  auto* bookmarks = g_bookmark_file_new();
  const bool standard_format = g_bookmark_file_load_from_file(bookmarks, history.c_str(), nullptr);
  g_bookmark_file_free(bookmarks);
  return standard_format ? 0 : 4;
}
