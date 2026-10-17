#include "desktop_launcher.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

bool check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

int main() {
  char pattern[] = "/tmp/midnight-launcher-test-XXXXXX";
  const auto* directory = mkdtemp(pattern);
  if (!check(directory, "temporary directory creation failed")) return 1;
  const auto root = fs::path(directory);
  const auto desktop = root / "Desktop";
  const auto downloads = root / "Downloads";
  fs::create_directories(desktop);
  fs::create_directories(downloads);
  const auto entry = desktop / "example.desktop";
  {
    std::ofstream file(entry);
    file << "[Desktop Entry]\nType=Application\nName=Example Launcher\n"
            "Icon=utilities-terminal\nExec=/usr/bin/true\nActions=new;missing;unnamed;\n"
            "[Desktop Action new]\nName=New window\nIcon=window-new\nExec=/usr/bin/touch "
         << (root / "action.launched").string() << "\n"
            "[Desktop Action missing]\nName=Missing command\n"
            "[Desktop Action unnamed]\nExec=/usr/bin/true\n"
            "[Desktop Action hidden]\nName=Not listed\nExec=/usr/bin/true\n";
  }

  auto info = midnight::desktop_launcher_info(entry, desktop);
  if (!check(info && info->name == "Example Launcher", "launcher name not read")) return 1;
  if (!check(info->icon == "utilities-terminal", "launcher icon not read")) return 1;
  if (!check(!info->trusted, "nonexecutable launcher trusted")) return 1;
  if (!check(!midnight::launch_desktop_entry(entry, desktop), "nonexecutable launcher ran")) return 1;

  auto actions = midnight::desktop_launcher_actions(entry, desktop);
  if (!check(actions && actions->size() == 1 && actions->front().id == "new" &&
             actions->front().name == "New window" && actions->front().icon == "window-new",
             "action metadata or missing-key filtering failed")) return 1;
  if (!check(!midnight::launch_desktop_entry_action(entry, desktop, "new"),
             "untrusted action ran")) return 1;
  chmod(entry.c_str(), 0700);
  info = midnight::desktop_launcher_info(entry, desktop);
  if (!check(info && info->trusted, "executable launcher not trusted")) return 1;
  if (!check(midnight::launch_desktop_entry(entry, desktop).has_value(), "trusted launcher failed")) return 1;

  if (!check(!midnight::launch_desktop_entry_action(entry, desktop, "hidden"),
             "unlisted action ran")) return 1;
  if (!check(midnight::launch_desktop_entry_action(entry, desktop, "new").has_value(),
             "trusted action failed")) return 1;
  for (int attempt = 0; attempt < 100 && !fs::exists(root / "action.launched"); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!check(fs::exists(root / "action.launched"), "action command did not run")) return 1;
  chmod(entry.c_str(), 0600);
  if (!check(!midnight::launch_desktop_entry_action(entry, desktop, "new"),
             "revoked trust was not rechecked")) return 1;
  chmod(entry.c_str(), 0700);

  const auto downloaded = downloads / "example.desktop";
  fs::copy_file(entry, downloaded);
  chmod(downloaded.c_str(), 0700);
  info = midnight::desktop_launcher_info(downloaded, desktop);
  if (!check(info && !info->trusted, "downloaded launcher trusted")) return 1;
  if (!check(!midnight::launch_desktop_entry(downloaded, desktop), "downloaded launcher ran")) return 1;

  const auto linked = desktop / "linked.desktop";
  fs::create_symlink(entry, linked);
  info = midnight::desktop_launcher_info(linked, desktop);
  if (!check(info && !info->trusted, "symlink launcher trusted")) return 1;
  if (!check(!midnight::launch_desktop_entry(linked, desktop), "symlink launcher ran")) return 1;

  const auto invalid = desktop / "invalid.desktop";
  {
    std::ofstream file(invalid);
    file << "[Desktop Entry]\nType=Directory\nName=Wrong Type\n";
  }
  chmod(invalid.c_str(), 0700);
  if (!check(!midnight::desktop_launcher_info(invalid, desktop), "invalid launcher accepted")) return 1;
  if (!check(!midnight::launch_desktop_entry(invalid, desktop), "invalid launcher ran")) return 1;

  fs::remove_all(root);
}
