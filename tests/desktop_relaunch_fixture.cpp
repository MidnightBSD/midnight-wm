// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include <xcb/xcb.h>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
namespace {
volatile std::sig_atomic_t stopping{};
void stop(int) { stopping = 1; }
}
int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const std::string id = argv[2], mode = argv[3];
  auto* connection = xcb_connect(nullptr, nullptr);
  if (xcb_connection_has_error(connection)) { xcb_disconnect(connection); return 1; }
  auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
  const auto atom = [&](const char* name) {
    auto* reply = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, false, std::strlen(name), name), nullptr);
    const auto result = reply ? reply->atom : XCB_ATOM_NONE; std::free(reply); return result;
  };
  for (int i = 0; i < (mode == "multiple" ? 2 : 1); ++i) {
    const auto window = xcb_generate_id(connection);
    xcb_create_window(connection, screen->root_depth, window, screen->root, 100 + 40*i, 100, 320, 200,
      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, atom("_GTK_APPLICATION_ID"),
      mode == "wrong-type" ? XCB_ATOM_STRING : atom("UTF8_STRING"), 8, id.size(), id.data());
    // These must never be used as a fallback restart command or persisted.
    const char secret[] = "echo\0TOKEN=private-command-should-not-persist\0";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_COMMAND,
      XCB_ATOM_STRING, 8, sizeof(secret), secret);
    if (mode == "xsmp") {
      const char client[] = "unregistered-session-client";
      xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, atom("SM_CLIENT_ID"),
        XCB_ATOM_STRING, 8, sizeof(client)-1, client);
    }
    xcb_map_window(connection, window);
  }
  xcb_flush(connection);
  const auto report = std::filesystem::path(argv[1]) / (std::to_string(getpid()) + ".txt");
  {
    std::ofstream file(report);
    file << id << '\n' << mode << '\n';
    for (const char* name : {"MIDNIGHT_WM_WINDOW_FD", "MIDNIGHT_WM_WINDOW_REPORT", "MIDNIGHT_WM_RESTORE_PLAN"})
      file << name << '=' << (std::getenv(name) ? std::getenv(name) : "") << '\n';
  }
  std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
  while (!stopping && !xcb_connection_has_error(connection)) usleep(20000);
  xcb_disconnect(connection);
}
