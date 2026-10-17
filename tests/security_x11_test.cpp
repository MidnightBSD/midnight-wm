// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "startup_notification.hpp"
#include "clipboard.hpp"
#include "dnd.hpp"
#include <gio/gdesktopappinfo.h>
#include <xcb/xcb.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main() {
  auto* connection = xcb_connect(nullptr, nullptr);
  if (xcb_connection_has_error(connection)) return 1;
  auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
  auto atom = [&](const char* name) {
    auto* reply = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0,
        std::char_traits<char>::length(name), name), nullptr);
    const auto result = reply ? reply->atom : XCB_ATOM_NONE;
    std::free(reply);
    return result;
  };
  const auto client = xcb_generate_id(connection);
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, client, screen->root,
      0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
  {
    midnight::StartupNotification startup(connection, screen, 0);
    auto* keys = g_key_file_new();
    g_key_file_set_string(keys, "Desktop Entry", "Type", "Application");
    g_key_file_set_string(keys, "Desktop Entry", "Name", "Test");
    g_key_file_set_string(keys, "Desktop Entry", "Exec", "/usr/bin/true");
    g_key_file_set_string(keys, "Desktop Entry", "StartupWMClass", "malformed");
    auto* app = g_desktop_app_info_new_from_keyfile(keys);
    if (!app) return 1;
    const auto startup_id = startup.begin(G_APP_INFO(app));
    if (startup_id.empty()) return 1;
    // Deliberately omit both ICCCM NUL terminators.
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, client, XCB_ATOM_WM_CLASS,
        XCB_ATOM_STRING, 8, 9, "malformed");
    startup.window_mapped(client);
    if (startup.pending_count() != 0) {
      std::cerr << "unterminated WM_CLASS did not use a bounded name\n";
      return 1;
    }
    g_object_unref(app);
    g_key_file_unref(keys);
  }
  {
    midnight::X11Clipboard clipboard(connection, screen->root);
    if (!clipboard.initialize()) return 1;
    clipboard.set_files({std::filesystem::path("/" + std::string(70000, 'a'))}, false);
    const auto selection = atom("CLIPBOARD");
    for (int i = 0; i < 17; ++i) {
      const auto property = atom(("SECURITY_TRANSFER_" + std::to_string(i)).c_str());
      xcb_convert_selection(connection, client, selection, atom("UTF8_STRING"), property, XCB_CURRENT_TIME);
    }
    xcb_flush(connection);
    int accepted = 0, rejected = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (accepted + rejected < 17 && std::chrono::steady_clock::now() < deadline) {
      while (auto* event = xcb_poll_for_event(connection)) {
        if (!clipboard.handle(event) && (event->response_type & 0x7f) == XCB_SELECTION_NOTIFY) {
          const auto* notify = reinterpret_cast<xcb_selection_notify_event_t*>(event);
          if (notify->requestor == client) {
            if (notify->property) ++accepted;
            else ++rejected;
          }
        }
        std::free(event);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (accepted != 16 || rejected != 1) {
      std::cerr << "INCR limit: accepted=" << accepted << " rejected=" << rejected << '\n';
      return 1;
    }
    clipboard.tick(std::chrono::steady_clock::now() + std::chrono::seconds(31));
    xcb_convert_selection(connection, client, selection, atom("UTF8_STRING"),
        atom("SECURITY_RECOVERED"), XCB_CURRENT_TIME);
    xcb_flush(connection);
    bool recovered = false;
    const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!recovered && std::chrono::steady_clock::now() < recovery_deadline) {
      while (auto* event = xcb_poll_for_event(connection)) {
        if (!clipboard.handle(event) && (event->response_type & 0x7f) == XCB_SELECTION_NOTIFY) {
          const auto* notify = reinterpret_cast<xcb_selection_notify_event_t*>(event);
          recovered = notify->requestor == client && notify->property == atom("SECURITY_RECOVERED");
        }
        std::free(event);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!recovered) { std::cerr << "expired clipboard slots were not reclaimed\n"; return 1; }
  }
  {
    // Outgoing XDND data must use the same INCR limits as the clipboard.
    const auto source = xcb_generate_id(connection);
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, source, screen->root,
        20, 20, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
    xcb_map_window(connection, source);
    xcb_flush(connection);
    std::free(xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr));
    midnight::X11Dnd dnd(connection, screen->root);
    if (!dnd.initialize() ||
        !dnd.begin_drag(source, {std::filesystem::path("/" + std::string(70000, 'a'))}, false)) {
      std::cerr << "could not start XDND source\n";
      return 1;
    }
    const auto selection = atom("XdndSelection");
    const auto uri_list = atom("text/uri-list");
    auto request_all = [&](int count, const char* prefix) {
      for (int i = 0; i < count; ++i) {
        const auto property = atom((prefix + std::to_string(i)).c_str());
        xcb_convert_selection(connection, client, selection, uri_list, property, XCB_CURRENT_TIME);
      }
      xcb_flush(connection);
      int accepted = 0, rejected = 0;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (accepted + rejected < count && std::chrono::steady_clock::now() < deadline) {
        while (auto* event = xcb_poll_for_event(connection)) {
          if (!dnd.handle(event) && (event->response_type & 0x7f) == XCB_SELECTION_NOTIFY) {
            const auto* notify = reinterpret_cast<xcb_selection_notify_event_t*>(event);
            if (notify->requestor == client) {
              if (notify->property) ++accepted;
              else ++rejected;
            }
          }
          std::free(event);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return std::pair{accepted, rejected};
    };
    if (const auto [accepted, rejected] = request_all(17, "SECURITY_DND_"); accepted != 16 || rejected != 1) {
      std::cerr << "XDND INCR limit: accepted=" << accepted << " rejected=" << rejected << '\n';
      return 1;
    }
    if (const auto [accepted, rejected] = request_all(1, "SECURITY_DND_"); accepted != 0 || rejected != 1) {
      std::cerr << "duplicate XDND requestor/property was not rejected\n";
      return 1;
    }
    dnd.tick(std::chrono::steady_clock::now() + std::chrono::seconds(31));
    if (const auto [accepted, rejected] = request_all(1, "SECURITY_DND_RECOVERED_"); accepted != 1) {
      std::cerr << "expired XDND slots were not reclaimed\n";
      return 1;
    }
    xcb_destroy_window(connection, source);
  }
  xcb_destroy_window(connection, client);
  xcb_disconnect(connection);
  std::cout << "Malformed WM_CLASS, stalled clipboard, and XDND transfer checks passed.\n";
}
