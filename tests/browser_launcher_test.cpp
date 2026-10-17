#include "browser_launcher.hpp"

#include <gio/gio.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  char directory[] = "/tmp/midnight-browser-test-XXXXXX";
  char* root_name = mkdtemp(directory);
  if (!root_name) return 2;
  const fs::path root(root_name);
  const auto config = root / "config";
  const auto data = root / "data";
  fs::create_directories(config);
  fs::create_directories(data / "applications");
  g_setenv("XDG_CONFIG_HOME", config.c_str(), TRUE);
  g_setenv("XDG_CONFIG_DIRS", (root / "config-system").c_str(), TRUE);
  g_setenv("XDG_DATA_HOME", data.c_str(), TRUE);
  g_setenv("XDG_DATA_DIRS", (root / "data-system").c_str(), TRUE);
  g_setenv("XDG_CURRENT_DESKTOP", "Midnight", TRUE);
  std::ofstream(data / "applications/org.midnightbsd.Raphael.desktop")
    << "[Desktop Entry]\nType=Application\nName=Raphael\nExec=/usr/bin/true\n"
       "MimeType=x-scheme-handler/http;x-scheme-handler/https;\n";
  std::ofstream(data / "applications/test-browser.desktop")
    << "[Desktop Entry]\nType=Application\nName=Test Browser\nExec=/usr/bin/true\n"
       "MimeType=x-scheme-handler/http;x-scheme-handler/https;\n";
  const std::string scenario(argv[1]);
  const char* expected = "org.midnightbsd.Raphael.desktop";
  if (scenario == "default") {
    std::ofstream(config / "midnight-mimeapps.list")
      << "[Default Applications]\nx-scheme-handler/https=test-browser.desktop;\n";
    expected = "test-browser.desktop";
  } else if (scenario != "fallback") {
    fs::remove_all(root);
    return 2;
  }
  auto* app = midnight::preferred_web_browser();
  const std::string actual = app && g_app_info_get_id(app) ? g_app_info_get_id(app) : "(none)";
  if (app) g_object_unref(app);
  fs::remove_all(root);
  if (actual != expected) {
    std::cerr << "Expected " << expected << ", got " << actual << '\n';
    return 1;
  }
  return 0;
}
