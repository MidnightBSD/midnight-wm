// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "file_manager1.hpp"

#include <gio/gio.h>

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  std::vector<std::string> uris;
  for (int index = 1; index < (argc > 1 ? argc : 2); ++index) {
    auto* file = g_file_new_for_commandline_arg(argc > 1 ? argv[index] : g_get_home_dir());
    if (!g_file_is_native(file)) {
      std::cerr << "Only local folders can be opened by Midnight Workspace\n";
      g_object_unref(file);
      return 1;
    }
    char* uri = g_file_get_uri(file);
    uris.emplace_back(uri);
    g_free(uri);
    g_object_unref(file);
  }
  std::vector<const char*> pointers;
  for (const auto& uri : uris) pointers.push_back(uri.c_str());
  pointers.push_back(nullptr);

  GError* error = nullptr;
  auto* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) {
    std::cerr << (error ? error->message : "Session bus unavailable") << '\n';
    g_clear_error(&error);
    return 1;
  }
  auto* reply = g_dbus_connection_call_sync(bus, midnight::workspace_file_manager_name,
      midnight::workspace_file_manager_path, "org.freedesktop.FileManager1", "ShowFolders",
      g_variant_new("(^ass)", pointers.data(), ""), nullptr, G_DBUS_CALL_FLAGS_NONE,
      5000, nullptr, &error);
  g_object_unref(bus);
  if (!reply) {
    std::cerr << (error ? error->message : "Could not open folder") << '\n';
    g_clear_error(&error);
    return 1;
  }
  g_variant_unref(reply);
  return 0;
}
