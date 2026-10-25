// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
// Run in an isolated X server with midnight-wm already running.
#include <xcb/xcb.h>
#include <xcb/sync.h>
#include <xcb/xcb_icccm.h>
#include <chrono>
#include <algorithm>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
using namespace std::chrono_literals;
xcb_connection_t* connection;
xcb_window_t root;
xcb_atom_t atom(const char* name) {
  auto* reply = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, std::char_traits<char>::length(name), name), nullptr);
  if (!reply) throw std::runtime_error("atom lookup failed");
  auto value = reply->atom; std::free(reply); return value;
}
std::vector<std::uint32_t> property(xcb_window_t window, const char* name) {
  auto* reply = xcb_get_property_reply(connection, xcb_get_property(connection, 0, window, atom(name), XCB_GET_PROPERTY_TYPE_ANY, 0, 128), nullptr);
  std::vector<std::uint32_t> result;
  if (reply && reply->format == 32) {
    const auto* data = static_cast<std::uint32_t*>(xcb_get_property_value(reply));
    result.assign(data, data + xcb_get_property_value_length(reply) / 4);
  }
  std::free(reply); return result;
}
void message(xcb_window_t window, const char* name, std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0) {
  xcb_client_message_event_t event{};
  event.response_type = XCB_CLIENT_MESSAGE; event.format = 32; event.window = window;
  event.type = atom(name); event.data.data32[0] = a; event.data.data32[1] = b; event.data.data32[2] = c;
  xcb_send_event(connection, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 reinterpret_cast<const char*>(&event)); xcb_flush(connection);
}
template<class F> void await(F test, const char* description) {
  for (int i = 0; i < 300; ++i) { if (test()) return; std::this_thread::sleep_for(10ms); }
  throw std::runtime_error(description);
}
bool viewable(xcb_window_t window) {
  auto* r = xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, window), nullptr);
  const bool value = r && r->map_state == XCB_MAP_STATE_VIEWABLE; std::free(r); return value;
}
int main() {
 try {
  connection = xcb_connect(nullptr, nullptr);
  root = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
  if (xcb_connection_has_error(connection)) throw std::runtime_error("no display");
  await([]{ return property(root, "_NET_NUMBER_OF_DESKTOPS") == std::vector<std::uint32_t>{4}; }, "desktop count");
  if (property(root, "_NET_WORKAREA").size() != 16) throw std::runtime_error("workarea count");
  // Portal dialogs can have no valid parent and minimum sizes above 720p.
  auto dialog = xcb_generate_id(connection);
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, dialog, root, 100, 100, 2000, 1200, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
  const auto dialog_type = atom("_NET_WM_WINDOW_TYPE_DIALOG");
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, dialog, atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM, 32, 1, &dialog_type);
  xcb_size_hints_t dialog_hints{};
  dialog_hints.flags = XCB_ICCCM_SIZE_HINT_P_MIN_SIZE |
      XCB_ICCCM_SIZE_HINT_BASE_SIZE | XCB_ICCCM_SIZE_HINT_P_RESIZE_INC;
  dialog_hints.base_width = dialog_hints.base_height = (-2147483647 - 1);
  dialog_hints.width_inc = dialog_hints.height_inc = 1;
  dialog_hints.min_width = 1600; dialog_hints.min_height = 900;
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, dialog, XCB_ATOM_WM_NORMAL_HINTS,
                      XCB_ATOM_WM_SIZE_HINTS, 32, sizeof(dialog_hints) / 4, &dialog_hints);
  xcb_map_window(connection, dialog); xcb_flush(connection);
  await([&]{ return viewable(dialog); }, "oversized dialog mapping");
  auto dialog_fits = [&]{
    auto* tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, dialog), nullptr);
    if (!tree) return false;
    const auto parent = tree->parent; std::free(tree);
    auto* outer = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, parent), nullptr);
    auto* inner = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, dialog), nullptr);
    const bool fits = outer && inner && outer->x >= 0 && outer->y >= 30 &&
        int(outer->x) + outer->width <= 1280 && int(outer->y) + outer->height <= 720 &&
        inner->width <= 1270 && inner->height <= 657;
    std::free(outer); std::free(inner); return fits;
  };
  await(dialog_fits, "initial oversized dialog did not fit usable screen");
  const std::uint32_t huge[] = {1100, 680, 3000, 2000};
  xcb_configure_window(connection, dialog, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                      XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, huge);
  xcb_flush(connection); std::this_thread::sleep_for(200ms);
  if (!dialog_fits()) throw std::runtime_error("dialog ConfigureRequest escaped usable screen");
  dialog_hints.min_width = 300; dialog_hints.min_height = 200;
  dialog_hints.flags |= XCB_ICCCM_SIZE_HINT_P_ASPECT;
  dialog_hints.min_aspect_num = dialog_hints.max_aspect_num = 2;
  dialog_hints.min_aspect_den = dialog_hints.max_aspect_den = 1;
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, dialog, XCB_ATOM_WM_NORMAL_HINTS,
                      XCB_ATOM_WM_SIZE_HINTS, 32, sizeof(dialog_hints) / 4, &dialog_hints);
  xcb_configure_window(connection, dialog, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                      XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, huge);
  xcb_flush(connection); std::this_thread::sleep_for(200ms);
  if (!dialog_fits()) throw std::runtime_error("dialog hints escaped usable screen");
  auto* fitted = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, dialog), nullptr);
  const bool hints_kept = fitted && fitted->width >= 300 && fitted->height >= 200 &&
      std::abs(int(fitted->width) - 2 * int(fitted->height)) <= 1;
  std::free(fitted);
  if (!hints_kept) throw std::runtime_error("feasible dialog minimum/aspect ignored");
  xcb_destroy_window(connection, dialog); xcb_flush(connection);
  auto client = xcb_generate_id(connection);
  const std::uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, client, root, 100, 100, 300, 200, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask);
  auto counter = xcb_generate_id(connection);
  xcb_sync_initialize_reply_t* init = xcb_sync_initialize_reply(connection, xcb_sync_initialize(connection, 3, 1), nullptr);
  std::free(init);
  xcb_sync_create_counter(connection, counter, {0, 77});
  const std::uint32_t protocols[] = {atom("_NET_WM_PING"), atom("_NET_WM_SYNC_REQUEST")};
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, client, atom("WM_PROTOCOLS"), XCB_ATOM_ATOM, 32, 2, protocols);
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, client, atom("_NET_WM_SYNC_REQUEST_COUNTER"), XCB_ATOM_CARDINAL, 32, 1, &counter);
  xcb_map_window(connection, client); xcb_flush(connection);
  await([&]{ return viewable(client); }, "initial mapping");
  await([&]{ return property(client, "_NET_WM_DESKTOP") == std::vector<std::uint32_t>{0}; }, "initial desktop");
  message(root, "_NET_CURRENT_DESKTOP", 1);
  await([&]{ return !viewable(client); }, "workspace switch should hide client");
  if (property(client, "_NET_WM_STATE") == std::vector<std::uint32_t>{atom("_NET_WM_STATE_HIDDEN")})
    throw std::runtime_error("workspace-hidden window marked minimized");
  message(client, "_NET_WM_DESKTOP", 1);
  await([&]{ return viewable(client); }, "move to current desktop");
  message(client, "_NET_WM_DESKTOP", 0xffffffffU);
  message(root, "_NET_CURRENT_DESKTOP", 2);
  await([]{ return property(root, "_NET_CURRENT_DESKTOP") == std::vector<std::uint32_t>{2}; }, "desktop 2");
  if (!viewable(client)) throw std::runtime_error("sticky window disappeared");
  message(root, "_NET_SHOWING_DESKTOP", 1);
  await([&]{ return !viewable(client); }, "show desktop hide");
  message(root, "_NET_SHOWING_DESKTOP", 0);
  await([&]{ return viewable(client); }, "show desktop restore");
  message(client, "WM_CHANGE_STATE", 3);
  await([&]{ return !viewable(client); }, "minimize");
  message(root, "_NET_SHOWING_DESKTOP", 1);
  message(root, "_NET_SHOWING_DESKTOP", 0);
  std::this_thread::sleep_for(100ms);
  if (viewable(client)) throw std::runtime_error("show desktop restored minimized window");
  message(client, "_NET_ACTIVE_WINDOW", 2);
  await([&]{ return viewable(client); }, "restore minimized client");
  message(root, "_NET_CURRENT_DESKTOP", 999);
  std::this_thread::sleep_for(100ms);
  if (property(root, "_NET_CURRENT_DESKTOP") != std::vector<std::uint32_t>{2}) throw std::runtime_error("invalid desktop accepted");
  message(client, "_NET_WM_DESKTOP", 999);
  std::this_thread::sleep_for(100ms);
  if (property(client, "_NET_WM_DESKTOP") != std::vector<std::uint32_t>{0xffffffffU}) throw std::runtime_error("invalid client desktop accepted");
  // New normal windows leave Show Desktop, and initial workspace hints are honored.
  message(root, "_NET_SHOWING_DESKTOP", 1);
  await([&]{ return !viewable(client); }, "show desktop before new window");
  auto second = xcb_generate_id(connection);
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, second, root, 450, 100, 200, 100, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
  xcb_map_window(connection, second); xcb_flush(connection);
  await([&]{ return viewable(second) && viewable(client); }, "new window should exit show desktop");
  auto third = xcb_generate_id(connection);
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, third, root, 500, 200, 200, 100, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
  const std::uint32_t desktop_three = 3;
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, third, atom("_NET_WM_DESKTOP"), XCB_ATOM_CARDINAL, 32, 1, &desktop_three);
  xcb_map_window(connection, third); xcb_flush(connection);
  await([&]{ auto list = property(root, "_NET_CLIENT_LIST"); return std::find(list.begin(), list.end(), third) != list.end(); }, "third window adoption");
  if (viewable(third)) throw std::runtime_error("initial off-workspace hint ignored");
  message(third, "_NET_ACTIVE_WINDOW", 2);
  await([&]{ return viewable(third) && property(root, "_NET_CURRENT_DESKTOP") == std::vector<std::uint32_t>{3}; }, "activation should switch workspace");
  auto child = xcb_generate_id(connection);
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, child, root, 600, 300, 100, 60, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, child, atom("WM_TRANSIENT_FOR"), XCB_ATOM_WINDOW, 32, 1, &third);
  xcb_map_window(connection, child); xcb_flush(connection);
  await([&]{ return viewable(child); }, "transient adoption");
  message(third, "_NET_WM_DESKTOP", 0);
  await([&]{ return !viewable(child) && !viewable(third) && property(child, "_NET_WM_DESKTOP") == std::vector<std::uint32_t>{0}; }, "transient family desktop move");
  xcb_destroy_window(connection, child); xcb_destroy_window(connection, third); xcb_destroy_window(connection, second);
  message(root, "_NET_CURRENT_DESKTOP", 2);
  message(client, "_NET_ACTIVE_WINDOW", 2);
  await([&]{ return property(root, "_NET_ACTIVE_WINDOW") == std::vector<std::uint32_t>{client}; }, "restore test focus");
  if (std::system("xdotool key ctrl+alt+Right") != 0) throw std::runtime_error("workspace shortcut failed");
  await([]{ return property(root, "_NET_CURRENT_DESKTOP") == std::vector<std::uint32_t>{3}; }, "workspace keyboard shortcut");
  if (std::system("xdotool key ctrl+alt+Left") != 0) throw std::runtime_error("workspace back shortcut failed");
  await([]{ return property(root, "_NET_CURRENT_DESKTOP") == std::vector<std::uint32_t>{2}; }, "workspace back keyboard shortcut");
  if (std::system("xdotool key super+d") != 0) throw std::runtime_error("show desktop shortcut failed");
  await([&]{ return !viewable(client); }, "show desktop keyboard shortcut");
  if (std::system("xdotool key super+d") != 0) throw std::runtime_error("show desktop restore shortcut failed");
  await([&]{ return viewable(client); }, "show desktop keyboard restore");
  // Keep a real ping unanswered to test timeout and late-response recovery.
  xcb_client_message_event_t ping{};
  await([&]{
    while (auto* event = xcb_poll_for_event(connection)) {
      if ((event->response_type & 127) == XCB_CLIENT_MESSAGE) {
        auto* m = reinterpret_cast<xcb_client_message_event_t*>(event);
        if (m->type == atom("WM_PROTOCOLS") && m->data.data32[0] == atom("_NET_WM_PING")) ping = *m;
      }
      std::free(event);
    }
    return ping.data.data32[1] != 0;
  }, "ping not received");
  std::this_thread::sleep_for(5200ms);
  auto visible_name = [&]{
    auto* r = xcb_get_property_reply(connection, xcb_get_property(connection, 0, client,
        atom("_NET_WM_VISIBLE_NAME"), XCB_GET_PROPERTY_TYPE_ANY, 0, 128), nullptr);
    std::string name;
    if (r && r->format == 8) name.assign(static_cast<char*>(xcb_get_property_value(r)), xcb_get_property_value_length(r));
    std::free(r); return name;
  };
  await([&]{ return visible_name().find("Not responding") != std::string::npos; }, "ping timeout indication");
  ping.window = root;
  xcb_send_event(connection, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 reinterpret_cast<const char*>(&ping)); xcb_flush(connection);
  await([&]{ return visible_name().empty(); }, "late ping recovery");
  // Start keyboard resizing, then send two right arrows without acknowledging the first resize.
  message(client, "_NET_WM_MOVERESIZE", 0, 0, 9);
  std::this_thread::sleep_for(100ms);
  if (std::system("xdotool key Right") != 0) throw std::runtime_error("xdotool Right failed");
  std::uint64_t requested = 0;
  await([&]{
    while (auto* event = xcb_poll_for_event(connection)) {
      if ((event->response_type & 127) == XCB_CLIENT_MESSAGE) {
        auto* m = reinterpret_cast<xcb_client_message_event_t*>(event);
        if (m->type == atom("WM_PROTOCOLS") && m->data.data32[0] == atom("_NET_WM_SYNC_REQUEST"))
          requested = (std::uint64_t(m->data.data32[3]) << 32) | m->data.data32[2];
      }
      std::free(event);
    }
    return requested != 0;
  }, "sync request not received");
  auto width = [&]{ auto* r = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, client), nullptr);
    const auto w = r ? r->width : 0; std::free(r); return w; };
  const auto first_width = width();
  if (std::system("xdotool key Right") != 0) throw std::runtime_error("second Right failed");
  std::this_thread::sleep_for(100ms);
  if (width() != first_width) throw std::runtime_error("resize was not throttled");
  xcb_sync_set_counter(connection, counter, {std::int32_t(requested >> 32), std::uint32_t(requested)}); xcb_flush(connection);
  await([&]{ return width() > first_width; }, "resize acknowledgement did not apply latest geometry");
  // A missing next acknowledgement must time out without wedging the WM.
  if (std::system("xdotool key Right") != 0) throw std::runtime_error("third Right failed");
  await([&]{ return width() >= first_width + 20; }, "sync timeout did not recover resize");
  if (std::system("xdotool key Return") != 0) throw std::runtime_error("Return failed");
  xcb_destroy_window(connection, client); xcb_sync_destroy_counter(connection, counter); xcb_flush(connection);
  // Frame input must resize the edge actually pressed (all eight directions).
  auto border_client = xcb_generate_id(connection);
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, border_client, root, 200, 200, 300, 180,
      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
  xcb_map_window(connection, border_client); xcb_flush(connection);
  await([&]{ return viewable(border_client); }, "border test mapping");
  auto* border_tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, border_client), nullptr);
  if (!border_tree) throw std::runtime_error("border frame missing");
  const auto border_frame = border_tree->parent; std::free(border_tree);
  for (int direction = 0; direction < 8; ++direction) {
    auto* before = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, border_frame), nullptr);
    if (!before) throw std::runtime_error("border frame geometry missing");
    const bool left = direction == 0 || direction == 6 || direction == 7;
    const bool right = direction == 2 || direction == 3 || direction == 4;
    const bool top = direction <= 2;
    const bool bottom = direction >= 4 && direction <= 6;
    const int x = left ? 0 : right ? before->width - 1 : before->width / 2;
    const int y = top ? 0 : bottom ? before->height - 1 : before->height / 2;
    xcb_button_press_event_t press{};
    press.response_type = XCB_BUTTON_PRESS; press.detail = 1;
    press.event = border_frame; press.root = root;
    press.event_x = x; press.event_y = y;
    press.root_x = before->x + x; press.root_y = before->y + y;
    xcb_send_event(connection, 0, border_frame, XCB_EVENT_MASK_BUTTON_PRESS,
        reinterpret_cast<const char*>(&press)); xcb_flush(connection);
    std::this_thread::sleep_for(40ms);
    xcb_motion_notify_event_t motion{};
    motion.response_type = XCB_MOTION_NOTIFY; motion.event = border_frame; motion.root = root;
    motion.root_x = press.root_x + 10; motion.root_y = press.root_y + 10;
    xcb_send_event(connection, 0, border_frame, XCB_EVENT_MASK_POINTER_MOTION,
        reinterpret_cast<const char*>(&motion));
    auto release = press; release.response_type = XCB_BUTTON_RELEASE;
    xcb_send_event(connection, 0, border_frame, XCB_EVENT_MASK_BUTTON_RELEASE,
        reinterpret_cast<const char*>(&release)); xcb_flush(connection);
    const int wanted_w = before->width + (left ? -10 : right ? 10 : 0);
    const int wanted_h = before->height + (top ? -10 : bottom ? 10 : 0);
    const int wanted_x = before->x + (left ? 10 : 0), wanted_y = before->y + (top ? 10 : 0);
    std::free(before);
    await([&]{
      auto* after = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, border_frame), nullptr);
      const bool matches = after && after->width == wanted_w && after->height == wanted_h &&
          after->x == wanted_x && after->y == wanted_y;
      std::free(after); return matches;
    }, "frame resized from incorrect edge");
  }
  auto closing = xcb_generate_id(connection);
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, closing, root, 400, 300, 200, 120,
      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
  xcb_map_window(connection, closing); xcb_flush(connection);
  await([&]{ return property(root, "_NET_ACTIVE_WINDOW") == std::vector<std::uint32_t>{closing}; }, "new client focus");
  xcb_destroy_window(connection, closing); xcb_flush(connection);
  await([&]{ return property(root, "_NET_ACTIVE_WINDOW") == std::vector<std::uint32_t>{border_client}; }, "focus fallback after closing client");
  // Change the physical keycode for a global shortcut on this private server.
  if (std::system("xmodmap -e 'keysym d = F12' -e 'keycode 200 = d'") != 0)
    throw std::runtime_error("keymap change failed");
  std::this_thread::sleep_for(200ms);
  if (std::system("xdotool key super+d") != 0) throw std::runtime_error("remapped shortcut failed");
  await([&]{ return property(root, "_NET_SHOWING_DESKTOP") == std::vector<std::uint32_t>{1}; }, "shortcut grab not refreshed");
  if (std::system("xdotool key super+d") != 0) throw std::runtime_error("remapped shortcut restore failed");
  await([&]{ return property(root, "_NET_SHOWING_DESKTOP") == std::vector<std::uint32_t>{0}; }, "remapped shortcut restoration");
  // _NET_CLOSE_WINDOW naming an unmanaged XID (a frame or the support window)
  // must be ignored rather than reaching KillClient on the WM's own connection.
  const auto support = property(root, "_NET_SUPPORTING_WM_CHECK");
  if (support.size() != 1) throw std::runtime_error("support window missing");
  message(border_frame, "_NET_CLOSE_WINDOW", 0, 2);
  message(support.front(), "_NET_CLOSE_WINDOW", 0, 2);
  message(root, "_NET_CLOSE_WINDOW", 0, 2);
  std::this_thread::sleep_for(300ms);
  if (!viewable(border_frame)) throw std::runtime_error("_NET_CLOSE_WINDOW on a frame killed the WM");
  if (property(root, "_NET_ACTIVE_WINDOW") != std::vector<std::uint32_t>{border_client})
    throw std::runtime_error("WM lost state after hostile _NET_CLOSE_WINDOW");
  // _NET_WM_MOVERESIZE without a held button must not take input grabs.
  message(border_client, "_NET_WM_MOVERESIZE", 300, 300, 8);
  std::this_thread::sleep_for(200ms);
  {
    auto* grab = xcb_grab_pointer_reply(connection, xcb_grab_pointer(connection, 0, root, 0,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), nullptr);
    const bool free_pointer = grab && grab->status == XCB_GRAB_STATUS_SUCCESS; std::free(grab);
    xcb_ungrab_pointer(connection, XCB_CURRENT_TIME); xcb_flush(connection);
    if (!free_pointer) throw std::runtime_error("_NET_WM_MOVERESIZE without a button grabbed the pointer");
  }
  xcb_destroy_window(connection, border_client); xcb_flush(connection);
  xcb_disconnect(connection);
  std::cout << "Dialog sizing/hints, workspaces, sticky windows, show desktop, invalid requests, ping timeout/recovery, synchronized resize and timeout, hostile close/moveresize passed.\n";
  return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
