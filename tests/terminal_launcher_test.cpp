#include "terminal_launcher.hpp"

#include <gio/gio.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  char directory[] = "/tmp/midnight-terminal-test-XXXXXX";
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
  g_setenv("XDG_CURRENT_DESKTOP", "MidnightWorkspace", TRUE);
  g_unsetenv("TERMINAL");
  std::ofstream(data / "applications/xterm.desktop")
    << "[Desktop Entry]\nType=Application\nName=XTerm\nExec=/usr/bin/true\n"
       "Categories=System;TerminalEmulator;\n";
  std::ofstream(data / "applications/test-terminal.desktop")
    << "[Desktop Entry]\nType=Application\nName=Test Terminal\nExec=/usr/bin/true\n"
       "Categories=System;TerminalEmulator;\nNoDisplay=true\n";
  const std::string scenario(argv[1]);
  const char* expected = "xterm.desktop";
  if (scenario == "default") {
    std::ofstream(config / "midnightworkspace-xdg-terminals.list")
      << "# Preferred terminal\nmissing.desktop\ntest-terminal.desktop\n";
    expected = "test-terminal.desktop";
  } else if (scenario == "env") {
    g_setenv("TERMINAL", "/usr/bin/true", TRUE);
    expected = "/usr/bin/true";
  } else if (scenario != "fallback") {
    fs::remove_all(root);
    return 2;
  }
  auto* app = midnight::preferred_terminal();
  const std::string actual = scenario == "env" ?
    (app && g_app_info_get_executable(app) ? g_app_info_get_executable(app) : "(none)") :
    (app && g_app_info_get_id(app) ? g_app_info_get_id(app) : "(none)");
  if (app) g_object_unref(app);
  fs::remove_all(root);
  if (actual != expected) {
    std::cerr << "Expected " << expected << ", got " << actual << '\n';
    return 1;
  }
  return 0;
}
