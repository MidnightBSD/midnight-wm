// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include "autostart.hpp"
#include "startup_applications.hpp"

#include <glib.h>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void entry(const std::filesystem::path& root, const std::string& id, const std::string& extra = "") {
  std::filesystem::create_directories(root / "autostart");
  std::ofstream(root / "autostart" / id) <<
      "# preserve this comment\n[Desktop Entry]\nType=Application\nName=Example\nExec=/usr/bin/true\n" << extra;
}

const midnight::StartupApplication* find(const std::vector<midnight::StartupApplication>& applications,
                                          const std::string& id) {
  for (const auto& application : applications) if (application.id == id) return &application;
  return nullptr;
}
} // namespace

int main() {
  g_setenv("XDG_CURRENT_DESKTOP", "MidnightWorkspace", true);
  GError* error = nullptr;
  char* temporary = g_dir_make_tmp("midnight-startup-settings-XXXXXX", &error);
  if (!temporary) {
    std::cerr << (error ? error->message : "Could not create temporary directory") << '\n';
    g_clear_error(&error);
    return 1;
  }
  const std::filesystem::path root(temporary);
  g_free(temporary);
  const auto user = root / "user";
  const auto system = root / "system";
  entry(system, "system.desktop", "X-Custom=retained\n");
  entry(system, "excluded.desktop", "OnlyShowIn=OtherDesktop;\n");
  auto applications = midnight::startup_applications({user, system});
  const auto* system_entry = find(applications, "system.desktop");
  if (!system_entry || !system_entry->enabled || system_entry->user_entry ||
      !find(applications, "excluded.desktop") || find(applications, "excluded.desktop")->available) return 1;
  if (auto result = midnight::set_startup_application_enabled(*system_entry, false, user); !result) return 1;
  applications = midnight::startup_applications({user, system});
  const auto* override = find(applications, "system.desktop");
  if (!override || override->enabled || !override->available || !override->user_entry ||
      !midnight::find_autostart_entries({user, system}).empty()) return 1;
  std::ifstream override_file(user / "autostart/system.desktop");
  const std::string override_text((std::istreambuf_iterator<char>(override_file)), {});
  if (override_text.find("preserve this comment") == std::string::npos ||
      override_text.find("X-Custom=retained") == std::string::npos) return 1;
  if (auto result = midnight::set_startup_application_enabled(*override, true, user); !result) return 1;
  if (midnight::find_autostart_entries({user, system}).size() != 1) return 1;
  const auto added = midnight::add_startup_application("Another", "/usr/bin/true --example", user);
  if (!added || added->ends_with(".desktop") == false) return 1;
  applications = midnight::startup_applications({user, system});
  const auto* custom = find(applications, *added);
  if (!custom || !custom->enabled || !custom->user_entry ||
      midnight::find_autostart_entries({user, system}).size() != 2) return 1;
  if (midnight::add_startup_application("Bad", "line\nbreak", user)) return 1;
  if (auto result = midnight::remove_startup_application(*custom, user); !result) return 1;
  if (midnight::find_autostart_entries({user, system}).size() != 1) return 1;
  applications = midnight::startup_applications({user, system});
  override = find(applications, "system.desktop");
  if (!override || !midnight::remove_startup_application(*override, user)) return 1;
  if (midnight::find_autostart_entries({user, system}).size() != 1) return 1;
  std::filesystem::remove_all(root);
  return 0;
}
