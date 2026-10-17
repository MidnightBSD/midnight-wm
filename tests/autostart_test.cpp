#include "autostart.hpp"

#include <glib.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace {
void write_entry(const std::filesystem::path& directory, const std::string& name,
                 const std::string& fields) {
  std::filesystem::create_directories(directory / "autostart");
  std::ofstream file(directory / "autostart" / name);
  file << "[Desktop Entry]\nType=Application\nName=Test\nExec=/usr/bin/true\n" << fields;
}
} // namespace

int main() {
  g_setenv("XDG_CURRENT_DESKTOP", "MidnightWorkspace", true);
  GError* error = nullptr;
  char* directory = g_dir_make_tmp("midnight-autostart-XXXXXX", &error);
  if (!directory) {
    std::cerr << (error ? error->message : "temporary directory failed") << '\n';
    if (error) g_error_free(error);
    return 1;
  }
  const std::filesystem::path root(directory);
  g_free(directory);
  const auto user = root / "user";
  const auto system = root / "system";

  write_entry(user, "regular.desktop", "");
  write_entry(user, "hidden.desktop", "Hidden=true\n");
  write_entry(system, "hidden.desktop", "");
  write_entry(user, "other.desktop", "OnlyShowIn=OtherDesktop;\n");
  write_entry(system, "other.desktop", "");
  write_entry(user, "excluded.desktop", "NotShowIn=MidnightWorkspace;\n");
  write_entry(user, "missing.desktop", "TryExec=/midnight-wm-test/nonexistent\n");
  write_entry(user, "nodisplay.desktop", "NoDisplay=true\n");
  write_entry(system, "system.desktop", "OnlyShowIn=MidnightWorkspace;\n");

  const auto entries = midnight::find_autostart_entries({user, system});
  std::set<std::string> names;
  for (const auto& entry : entries) names.insert(entry.filename().string());
  std::filesystem::remove_all(root);

  if (names != std::set<std::string>{"nodisplay.desktop", "regular.desktop", "system.desktop"}) {
    std::cerr << "Unexpected autostart selection:";
    for (const auto& name : names) std::cerr << ' ' << name;
    std::cerr << '\n';
    return 1;
  }
  return 0;
}
