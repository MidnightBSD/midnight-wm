// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include <xcb/xcb.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main() {
  auto* connection = xcb_connect(nullptr, nullptr);
  if (xcb_connection_has_error(connection)) return 1;
  auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
  constexpr char name[] = "_NET_WM_ICON";
  auto* reply = xcb_intern_atom_reply(connection,
      xcb_intern_atom(connection, false, sizeof(name) - 1, name), nullptr);
  if (!reply) return 1;
  const auto icon_atom = reply->atom;
  std::free(reply);
  std::vector<xcb_window_t> windows;
  const auto icon = [&](xcb_window_t window, std::uint32_t color) {
    std::vector<std::uint32_t> pixels{32, 32};
    pixels.insert(pixels.end(), 32 * 32, color);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, icon_atom,
                        XCB_ATOM_CARDINAL, 32, pixels.size(), pixels.data());
  };
  for (int i = 0; i < 12; ++i) {
    const auto window = xcb_generate_id(connection);
    const std::uint32_t background[]{0xdddddd};
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      160 + i * 14, 150 + i * 12, 240, 120, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL, background);
    const auto title = "Task icon " + std::to_string(i + 1);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, title.size(), title.data());
    constexpr char wm_class[] = "fixture\0TaskIconFixture\0";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS,
                        XCB_ATOM_STRING, 8, sizeof(wm_class) - 1, wm_class);
    icon(window, 0xffff2020);
    xcb_map_window(connection, window);
    windows.push_back(window);
  }
  xcb_flush(connection);
  for (std::string command; std::getline(std::cin, command);) {
    if (command == "green") icon(windows.front(), 0xff20ff20);
    else if (command == "delete") xcb_delete_property(connection, windows.front(), icon_atom);
    else if (command == "malformed") {
      const std::uint32_t pixels[]{0xffffffff, 0xffffffff, 0};
      xcb_change_property(connection, XCB_PROP_MODE_REPLACE, windows.front(), icon_atom,
                          XCB_ATOM_CARDINAL, 32, 3, pixels);
    } else if (command == "class") {
      constexpr char wm_class[] = "unmatched\0NoMatchingApplication\0";
      xcb_change_property(connection, XCB_PROP_MODE_REPLACE, windows.front(), XCB_ATOM_WM_CLASS,
                          XCB_ATOM_STRING, 8, sizeof(wm_class) - 1, wm_class);
    } else if (command == "quit") break;
    xcb_flush(connection);
  }
  xcb_disconnect(connection);
}
