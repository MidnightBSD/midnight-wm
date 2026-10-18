// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "session_bus.hpp"

#include <gio/gio.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

namespace midnight {
namespace {
bool can_connect_to_bus(const char* addresses) {
  if (!addresses || !*addresses) return false;
  char** parts = g_strsplit(addresses, ",", -1);
  bool connected = false;
  for (char** part = parts; part && *part; ++part) {
    // The autolaunch transport can create a bus during this check. Let the
    // managed dbus-run-session wrapper do that instead.
    if (!**part || g_str_has_prefix(*part, "autolaunch:")) continue;
    GError* error = nullptr;
    auto* bus = g_dbus_connection_new_for_address_sync(
        *part, static_cast<GDBusConnectionFlags>(
                   G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    if (error) g_error_free(error);
    if (!bus) continue;
    g_dbus_connection_set_exit_on_close(bus, FALSE);
    connected = true;
    (void)g_dbus_connection_close_sync(bus, nullptr, nullptr);
    g_object_unref(bus);
    break;
  }
  g_strfreev(parts);
  return connected;
}

} // namespace

void ensure_session_bus(int argc, char** argv) {
  if (const char* address = g_getenv("DBUS_SESSION_BUS_ADDRESS"); can_connect_to_bus(address)) {
    g_unsetenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED");
    return;
  }

  // A display manager can provide a bus at the standard runtime path without
  // exporting its address. Give GIO and launched apps the same address.
  if (const char* runtime = g_getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
    char* path = g_build_filename(runtime, "bus", nullptr);
    char* escaped = g_dbus_address_escape_value(path);
    const std::string address = std::string("unix:path=") + escaped;
    g_free(escaped);
    g_free(path);
    if (can_connect_to_bus(address.c_str())) {
      g_setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), TRUE);
      g_unsetenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED");
      return;
    }
  }

  // The child inherits this marker so a failed wrapper cannot recurse.
  if (g_getenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED")) {
    g_unsetenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED");
    std::cerr << "D-Bus session unavailable after startup\n";
    return;
  }
  char* wrapper = g_find_program_in_path("dbus-run-session");
  char* daemon = g_find_program_in_path("dbus-daemon");
  if (!wrapper || !daemon) {
    std::cerr << "D-Bus session unavailable (dbus-run-session or dbus-daemon is missing)\n";
    g_free(wrapper);
    g_free(daemon);
    return;
  }
  g_free(daemon);
  g_setenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED", "1", TRUE);
  char separator[] = "--";
  std::vector<char*> arguments{wrapper, separator};
  for (int i = 0; i < argc; ++i) arguments.push_back(argv[i]);
  arguments.push_back(nullptr);
  execv(wrapper, arguments.data());
  const int failure = errno;
  g_unsetenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED");
  std::cerr << "Could not start D-Bus session: " << std::strerror(failure) << '\n';
  g_free(wrapper);
}
} // namespace midnight
