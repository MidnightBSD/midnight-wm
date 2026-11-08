#pragma once

#include <glib.h>

#include <string>

namespace midnight {

// Midnight Workspace prefers nightsaver and falls back to XScreenSaver.
// nightsaver speaks the XScreenSaver root-window protocol, so either command
// controls whichever compatible daemon is running.
struct ScreenSaverProgram {
  std::string path;
  bool nightsaver{};
};

inline ScreenSaverProgram find_screensaver_program(const char* preferred, const char* fallback) {
  for (const auto* name : {preferred, fallback}) {
    if (char* found = g_find_program_in_path(name)) {
      ScreenSaverProgram program{found, name == preferred};
      g_free(found);
      return program;
    }
  }
  return {};
}

inline ScreenSaverProgram find_screensaver_daemon() {
  return find_screensaver_program("nightsaver", "xscreensaver");
}

inline ScreenSaverProgram find_screensaver_command() {
  return find_screensaver_program("nightsaver-command", "xscreensaver-command");
}

} // namespace midnight
