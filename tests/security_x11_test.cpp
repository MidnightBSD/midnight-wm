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
#include <optional>
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
    auto event_mask = [&](xcb_window_t window) {
      auto* attributes = xcb_get_window_attributes_reply(connection,
          xcb_get_window_attributes(connection, window), nullptr);
      const uint32_t mask = attributes ? attributes->your_event_mask : 0;
      std::free(attributes);
      return mask;
    };
    auto pump_until = [&](auto predicate, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
      const auto deadline = std::chrono::steady_clock::now() + limit;
      while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        while (auto* event = xcb_poll_for_event(connection)) {
          (void)clipboard.handle(event);
          std::free(event);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return predicate();
    };
    {
      // An INCR transfer must not replace this connection's event selection on
      // the requestor, and the root window is never a legitimate requestor.
      const uint32_t root_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
      const uint32_t client_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE;
      xcb_change_window_attributes(connection, screen->root, XCB_CW_EVENT_MASK, &root_mask);
      xcb_change_window_attributes(connection, client, XCB_CW_EVENT_MASK, &client_mask);
      xcb_convert_selection(connection, screen->root, selection, atom("UTF8_STRING"),
          atom("SECURITY_ROOT"), XCB_CURRENT_TIME);
      xcb_flush(connection);
      // The reply to a server-owned window reaches no client; the observable
      // outcome is that this connection's root selection is untouched.
      (void)pump_until([&] { return event_mask(screen->root) != root_mask; }, std::chrono::milliseconds(300));
      if (event_mask(screen->root) != root_mask) { std::cerr << "clipboard changed the root event mask\n"; return 1; }
      xcb_convert_selection(connection, client, selection, atom("UTF8_STRING"), atom("SECURITY_MASK"), XCB_CURRENT_TIME);
      xcb_flush(connection);
      bool mask_notified = false;
      const auto mask_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!mask_notified && std::chrono::steady_clock::now() < mask_deadline) {
        while (auto* event = xcb_poll_for_event(connection)) {
          if (!clipboard.handle(event) && (event->response_type & 0x7f) == XCB_SELECTION_NOTIFY) {
            const auto* notify = reinterpret_cast<xcb_selection_notify_event_t*>(event);
            mask_notified = notify->requestor == client && notify->property == atom("SECURITY_MASK");
          }
          std::free(event);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!mask_notified || event_mask(client) != (client_mask | XCB_EVENT_MASK_PROPERTY_CHANGE)) {
        std::cerr << "INCR transfer replaced the requestor event mask\n"; return 1;
      }
      // Expire the pending slot so it does not count against the limit below.
      clipboard.tick(std::chrono::steady_clock::now() + std::chrono::seconds(31));
      const uint32_t none = 0;
      xcb_change_window_attributes(connection, screen->root, XCB_CW_EVENT_MASK, &none);
      xcb_change_window_attributes(connection, client, XCB_CW_EVENT_MASK, &none);
      xcb_flush(connection);
      clipboard.set_files({std::filesystem::path("/" + std::string(70000, 'a'))}, false);
    }
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
    {
      // An owner that never answers must not park paste requests forever.
      xcb_set_selection_owner(connection, client, selection, XCB_CURRENT_TIME);
      xcb_flush(connection);
      bool answered = false;
      std::size_t received = 1;
      // Discard SelectionRequests aimed at the stalled owner: the clipboard
      // must recover through its own deadline, not through a reply.
      const auto observed = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      while (std::chrono::steady_clock::now() < observed) {
        while (auto* event = xcb_poll_for_event(connection)) { (void)clipboard.handle(event); std::free(event); }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      clipboard.request_files([&](std::vector<std::filesystem::path> files, bool) {
        answered = true;
        received = files.size();
      });
      for (int step = 0; step < 8 && !answered; ++step) {
        clipboard.tick(std::chrono::steady_clock::now() + std::chrono::seconds(6));
        while (auto* event = xcb_poll_for_event(connection)) { (void)clipboard.handle(event); std::free(event); }
      }
      if (!answered || received != 0) { std::cerr << "stalled clipboard owner wedged the paste request\n"; return 1; }
      xcb_set_selection_owner(connection, XCB_WINDOW_NONE, selection, XCB_CURRENT_TIME);
      xcb_flush(connection);
    }
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
    auto request_all = [&](int count, const char* prefix, xcb_atom_t target) {
      for (int i = 0; i < count; ++i) {
        const auto property = atom((prefix + std::to_string(i)).c_str());
        xcb_convert_selection(connection, client, selection, target, property, XCB_CURRENT_TIME);
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
    if (const auto [accepted, rejected] = request_all(17, "SECURITY_DND_", uri_list); accepted != 16 || rejected != 1) {
      std::cerr << "XDND INCR limit: accepted=" << accepted << " rejected=" << rejected << '\n';
      return 1;
    }
    if (const auto [accepted, rejected] = request_all(1, "SECURITY_DND_", uri_list); accepted != 0 || rejected != 1) {
      std::cerr << "duplicate XDND requestor/property was not rejected\n";
      return 1;
    }
    if (const auto [accepted, rejected] = request_all(1, "SECURITY_DND_", atom("TARGETS")); accepted != 0 || rejected != 1) {
      std::cerr << "TARGETS reused an active XDND transfer property\n";
      return 1;
    }
    dnd.tick(std::chrono::steady_clock::now() + std::chrono::seconds(31));
    if (const auto [accepted, rejected] = request_all(1, "SECURITY_DND_RECOVERED_", uri_list); accepted != 1) {
      std::cerr << "expired XDND slots were not reclaimed\n";
      return 1;
    }
    xcb_destroy_window(connection, source);
  }
  {
    // Forged XdndEnter/Position/Drop messages from a client that does not own
    // XdndSelection must be refused before any file list is fetched.
    const auto target = xcb_generate_id(connection);
    const uint32_t target_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, target, screen->root,
        100, 100, 60, 60, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
        XCB_CW_EVENT_MASK, &target_mask);
    xcb_map_window(connection, target);
    xcb_flush(connection);
    midnight::X11Dnd dnd(connection, screen->root);
    if (!dnd.initialize()) return 1;
    bool dropped = false;
    dnd.register_target(target, [&](xcb_window_t, std::vector<std::filesystem::path>, bool, int, int,
                                    midnight::X11Dnd::DropFinish finish) { dropped = true; finish(true); });
    const auto uri_list = atom("text/uri-list");
    auto send = [&](const char* type, uint32_t d1, uint32_t d2 = 0, uint32_t d3 = 0, uint32_t d4 = 0) {
      xcb_client_message_event_t message{};
      message.response_type = XCB_CLIENT_MESSAGE;
      message.format = 32;
      message.window = target;
      message.type = atom(type);
      message.data.data32[0] = client;
      message.data.data32[1] = d1; message.data.data32[2] = d2;
      message.data.data32[3] = d3; message.data.data32[4] = d4;
      xcb_send_event(connection, 0, target, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&message));
      xcb_flush(connection);
    };
    auto wait_reply = [&](const char* type) -> std::optional<xcb_client_message_event_t> {
      const auto wanted = atom(type);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < deadline) {
        while (auto* event = xcb_poll_for_event(connection)) {
          // The handler claims every XDND-typed message, including replies
          // addressed to our source window, so inspect before dispatching.
          std::optional<xcb_client_message_event_t> message;
          if ((event->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
            const auto& candidate = *reinterpret_cast<xcb_client_message_event_t*>(event);
            if (candidate.window == client && candidate.type == wanted) message = candidate;
          }
          (void)dnd.handle(event);
          std::free(event);
          if (message) return message;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return std::nullopt;
    };
    xcb_warp_pointer(connection, XCB_WINDOW_NONE, screen->root, 0, 0, 0, 0, 10, 10);
    xcb_flush(connection);
    send("XdndEnter", 5U << 24, uri_list);
    send("XdndPosition", 0, (130U << 16) | 130U, 0, atom("XdndActionMove"));
    auto status = wait_reply("XdndStatus");
    if (!status || (status->data.data32[1] & 1U)) { std::cerr << "forged XdndPosition was accepted\n"; return 1; }
    send("XdndDrop", 0, XCB_CURRENT_TIME);
    auto finished = wait_reply("XdndFinished");
    if (!finished || (finished->data.data32[1] & 1U) || dropped) {
      std::cerr << "forged XdndDrop reached the drop handler\n"; return 1;
    }
    // A source that owns XdndSelection with the pointer over the target is a
    // real drag and must still be accepted.
    xcb_set_selection_owner(connection, client, atom("XdndSelection"), XCB_CURRENT_TIME);
    xcb_warp_pointer(connection, XCB_WINDOW_NONE, screen->root, 0, 0, 0, 0, 130, 130);
    xcb_flush(connection);
    send("XdndEnter", 5U << 24, uri_list);
    send("XdndPosition", 0, (130U << 16) | 130U, 0, atom("XdndActionCopy"));
    status = wait_reply("XdndStatus");
    if (!status || !(status->data.data32[1] & 1U)) { std::cerr << "genuine XdndPosition was refused\n"; return 1; }
    send("XdndLeave", 0);
    xcb_set_selection_owner(connection, XCB_WINDOW_NONE, atom("XdndSelection"), XCB_CURRENT_TIME);
    xcb_flush(connection);
    dnd.unregister_target(target);
    xcb_destroy_window(connection, target);
  }
  xcb_destroy_window(connection, client);
  xcb_disconnect(connection);
  std::cout << "Malformed WM_CLASS, clipboard mask/deadline, and XDND transfer/forgery checks passed.\n";
}
