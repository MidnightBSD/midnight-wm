// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
// Run only in the isolated server provided by ewmh_attention.py.
#include <xcb/xcb.h>
#include <xcb/xinerama.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
xcb_connection_t* connection;
xcb_window_t root;
xcb_atom_t atom(const char* name) {
  auto* r = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, std::char_traits<char>::length(name), name), nullptr);
  if (!r) throw std::runtime_error("atom lookup");
  auto a = r->atom; std::free(r); return a;
}
std::vector<std::uint32_t> property(xcb_window_t w, const char* name) {
  auto* r = xcb_get_property_reply(connection, xcb_get_property(connection, 0, w, atom(name), XCB_GET_PROPERTY_TYPE_ANY, 0, 256), nullptr);
  std::vector<std::uint32_t> result;
  if (r && r->format == 32) {
    auto* data = static_cast<std::uint32_t*>(xcb_get_property_value(r));
    result.assign(data, data + xcb_get_property_value_length(r) / 4);
  }
  std::free(r); return result;
}
void set(xcb_window_t w, const char* name, xcb_atom_t type, std::uint32_t value) {
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, w, atom(name), type, 32, 1, &value); xcb_flush(connection);
}
void message(xcb_window_t w, const char* name, std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0,
             std::uint32_t d = 0, std::uint32_t f = 0) {
  xcb_client_message_event_t e{}; e.response_type = XCB_CLIENT_MESSAGE; e.format = 32;
  e.window = w; e.type = atom(name); e.data.data32[0] = a; e.data.data32[1] = b; e.data.data32[2] = c;
  e.data.data32[3] = d; e.data.data32[4] = f;
  xcb_send_event(connection, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, reinterpret_cast<char*>(&e)); xcb_flush(connection);
}
template<class F> void await(F f, const char* error) {
  for (int i = 0; i < 250; ++i) { if (f()) return; std::this_thread::sleep_for(10ms); }
  throw std::runtime_error(error);
}
void check(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
bool has(xcb_window_t w, const char* name, const char* value) {
  auto p = property(w, name); return std::find(p.begin(), p.end(), atom(value)) != p.end();
}
bool state(xcb_window_t w, const char* value) { return has(w, "_NET_WM_STATE", value); }
bool active(xcb_window_t w) { return property(root, "_NET_ACTIVE_WINDOW") == std::vector<std::uint32_t>{w}; }
void focus(xcb_window_t w) { message(w, "_NET_ACTIVE_WINDOW", 2); await([&]{ return active(w); }, "pager activation"); }
void change(xcb_window_t w, std::uint32_t action, const char* a, const char* b = nullptr) {
  message(w, "_NET_WM_STATE", action, atom(a), b ? atom(b) : 0);
}
xcb_window_t create(bool map = true) {
  auto w = xcb_generate_id(connection);
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, w, root, 160, 160, 260, 170, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
  if (map) { xcb_map_window(connection, w); xcb_flush(connection); await([&]{ auto p = property(root, "_NET_CLIENT_LIST"); return std::find(p.begin(), p.end(), w) != p.end(); }, "client adoption"); }
  return w;
}
bool above(xcb_window_t a, xcb_window_t b) {
  auto p = property(root, "_NET_CLIENT_LIST_STACKING");
  auto ai = std::find(p.begin(), p.end(), a), bi = std::find(p.begin(), p.end(), b);
  return ai != p.end() && bi != p.end() && ai > bi;
}
// Shading clips the mapped client without changing its requested size.
void shade_requests() {
  check(has(root, "_NET_SUPPORTED", "_NET_WM_STATE_SHADED"), "SHADED not advertised");
  check(has(root, "_NET_SUPPORTED", "_NET_WM_ACTION_SHADE"), "SHADE action not advertised");
  auto window = create(false);
  set(window, "_NET_WM_STATE", XCB_ATOM_ATOM, atom("_NET_WM_STATE_SHADED"));
  xcb_map_window(connection, window); xcb_flush(connection);
  xcb_window_t frame = 0;
  await([&]{
    auto* tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, window), nullptr);
    if (tree) { frame = tree->parent; std::free(tree); }
    return frame != 0 && frame != root && state(window, "_NET_WM_STATE_SHADED");
  }, "initial shade not honored");
  auto geometry = [&](xcb_window_t target) {
    auto* reply = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, target), nullptr);
    check(reply != nullptr, "shade geometry query");
    std::vector<int> result{reply->x, reply->y, reply->width, reply->height};
    std::free(reply); return result;
  };
  auto shaded = [&]{ return state(window, "_NET_WM_STATE_SHADED") && geometry(frame)[3] == 28; };
  await(shaded, "shaded frame did not collapse to title height");
  const auto initial_frame = geometry(frame);
  const auto client = geometry(window);
  check(client[2] == 260 && client[3] == 170, "shading resized client");
  auto* attributes = xcb_get_window_attributes_reply(connection,
      xcb_get_window_attributes(connection, window), nullptr);
  check(attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE, "shading unmapped client");
  std::free(attributes);
  check(client[1] >= initial_frame[3], "shaded client not clipped below title");
  check(!state(window, "_NET_WM_STATE_HIDDEN"), "shade published HIDDEN");
  check(has(window, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_SHADE") &&
      !has(window, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_RESIZE"), "shaded allowed actions");
  change(window, 0, "_NET_WM_STATE_SHADED");
  await([&]{ return !state(window, "_NET_WM_STATE_SHADED") && geometry(frame)[3] > 170; }, "shade removal");
  const auto ordinary = geometry(frame);
  check(geometry(window) == client, "unshade changed client geometry");
  await([&]{ return has(window, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_RESIZE"); }, "unshade resize action");
  auto restore = [&]{
    await([&]{ return !state(window, "_NET_WM_STATE_SHADED") && geometry(frame) == ordinary; }, "shade geometry restoration");
    check(geometry(window) == client, "restored client geometry");
  };
  change(window, 2, "_NET_WM_STATE_SHADED", "_NET_WM_STATE_SHADED");
  await(shaded, "duplicate shade toggle applied twice");
  change(window, 2, "_NET_WM_STATE_SHADED"); restore();
  change(window, 9, "_NET_WM_STATE_SHADED");
  xcb_client_message_event_t malformed{};
  malformed.response_type = XCB_CLIENT_MESSAGE; malformed.format = 8;
  malformed.window = window; malformed.type = atom("_NET_WM_STATE");
  malformed.data.data32[0] = 1; malformed.data.data32[1] = atom("_NET_WM_STATE_SHADED");
  xcb_send_event(connection, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, reinterpret_cast<char*>(&malformed)); xcb_flush(connection);
  std::this_thread::sleep_for(100ms); restore();
  auto middle_click = [&]{
    const auto command = "xdotool mousemove --window " + std::to_string(frame) + " 80 12 click 2";
    check(std::system(command.c_str()) == 0, "middle-click title input");
  };
  middle_click(); await(shaded, "middle-click did not shade");
  middle_click(); restore();
  change(window, 1, "_NET_WM_STATE_SHADED"); await(shaded, "shade before workspace switch");
  message(root, "_NET_CURRENT_DESKTOP", 1);
  await([&]{ return property(root, "_NET_CURRENT_DESKTOP") == std::vector<std::uint32_t>{1}; }, "shaded workspace away");
  message(root, "_NET_CURRENT_DESKTOP", 0);
  await([&]{ return property(root, "_NET_CURRENT_DESKTOP") == std::vector<std::uint32_t>{0}; }, "shaded workspace return");
  check(shaded() && geometry(window) == client, "workspace switch lost shade geometry");
  message(root, "_NET_SHOWING_DESKTOP", 1);
  await([&]{ return property(root, "_NET_SHOWING_DESKTOP") == std::vector<std::uint32_t>{1}; }, "show desktop shaded");
  message(root, "_NET_SHOWING_DESKTOP", 0);
  await([&]{ return property(root, "_NET_SHOWING_DESKTOP") == std::vector<std::uint32_t>{0}; }, "restore desktop shaded");
  check(shaded(), "show desktop lost shade state");
  check(std::system("xdotool key alt+Tab") == 0, "Alt+Tab shade input");
  restore();
  change(window, 1, "_NET_WM_STATE_SHADED"); await(shaded, "shade before activation");
  focus(window); restore();
  change(window, 1, "_NET_WM_STATE_SHADED"); await(shaded, "shade before minimize");
  message(window, "WM_CHANGE_STATE", 3);
  await([&]{ return state(window, "_NET_WM_STATE_HIDDEN"); }, "minimize shaded window");
  focus(window); restore();
  check(!state(window, "_NET_WM_STATE_HIDDEN"), "restored shaded window still hidden");
  change(window, 1, "_NET_WM_STATE_SHADED"); await(shaded, "shade before fullscreen");
  change(window, 1, "_NET_WM_STATE_FULLSCREEN");
  await([&]{ return state(window, "_NET_WM_STATE_FULLSCREEN") && !state(window, "_NET_WM_STATE_SHADED"); }, "fullscreen did not unshade");
  change(window, 1, "_NET_WM_STATE_SHADED"); std::this_thread::sleep_for(100ms);
  check(!state(window, "_NET_WM_STATE_SHADED"), "fullscreen accepted shade");
  change(window, 0, "_NET_WM_STATE_FULLSCREEN"); restore();
  change(window, 1, "_NET_WM_STATE_SHADED"); await(shaded, "shade before maximize");
  change(window, 1, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ");
  await([&]{ return state(window, "_NET_WM_STATE_MAXIMIZED_VERT") && !state(window, "_NET_WM_STATE_SHADED"); }, "maximize did not unshade");
  change(window, 0, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"); restore();
  auto splash = create(false);
  set(splash, "_NET_WM_WINDOW_TYPE", XCB_ATOM_ATOM, atom("_NET_WM_WINDOW_TYPE_SPLASH"));
  set(splash, "_NET_WM_STATE", XCB_ATOM_ATOM, atom("_NET_WM_STATE_SHADED"));
  xcb_map_window(connection, splash); xcb_flush(connection);
  await([&]{ auto clients = property(root, "_NET_CLIENT_LIST");
    return std::find(clients.begin(), clients.end(), splash) != clients.end(); }, "splash adoption");
  change(splash, 1, "_NET_WM_STATE_SHADED"); std::this_thread::sleep_for(100ms);
  check(!state(splash, "_NET_WM_STATE_SHADED") &&
      !has(splash, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_SHADE"), "undecorated splash accepted shade");
  xcb_destroy_window(connection, splash); xcb_destroy_window(connection, window); xcb_flush(connection);
  std::cout << "Window shade geometry, input, state, and restore checks passed.\n";
}
// These requests must change stacking without changing the active client.
void protocol_requests() {
  check(has(root, "_NET_SUPPORTED", "_NET_RESTACK_WINDOW"), "RESTACK_WINDOW not advertised");
  check(has(root, "_NET_SUPPORTED", "_NET_WM_FULLSCREEN_MONITORS"), "FULLSCREEN_MONITORS not advertised");
  auto a = create(), b = create(), guard = create(); focus(guard);
  auto restack = [&](xcb_window_t target, xcb_window_t sibling, std::uint32_t mode,
                     std::uint32_t source = 2) {
    message(target, "_NET_RESTACK_WINDOW", source, sibling, mode);
  };
  auto order = [&](bool a_above, const char* error) {
    await([&]{ return a_above ? above(a, b) : above(b, a); }, error);
    check(active(guard), "restacking changed focus");
  };
  restack(a, b, XCB_STACK_MODE_ABOVE); order(true, "restack Above");
  restack(a, b, XCB_STACK_MODE_BELOW); order(false, "restack Below");
  // Both fixture windows overlap, making the conditional modes observable.
  restack(a, b, XCB_STACK_MODE_TOP_IF); order(true, "restack TopIf");
  restack(a, b, XCB_STACK_MODE_BOTTOM_IF); order(false, "restack BottomIf");
  restack(a, b, XCB_STACK_MODE_OPPOSITE); order(true, "restack Opposite raises");
  restack(a, b, XCB_STACK_MODE_OPPOSITE); order(false, "restack Opposite lowers");
  auto unmanaged = create(false);
  auto ignored = [&](xcb_window_t target, xcb_window_t sibling, std::uint32_t mode,
                     std::uint32_t source) {
    auto before = property(root, "_NET_CLIENT_LIST_STACKING");
    restack(target, sibling, mode, source); std::this_thread::sleep_for(100ms);
    check(property(root, "_NET_CLIENT_LIST_STACKING") == before && active(guard),
          "invalid restack changed stacking or focus");
  };
  ignored(a, b, 5, 2);
  for (auto source : {0U, 1U, 3U}) ignored(a, b, XCB_STACK_MODE_ABOVE, source);
  ignored(a, unmanaged, XCB_STACK_MODE_ABOVE, 2);
  ignored(unmanaged, b, XCB_STACK_MODE_ABOVE, 2);
  ignored(a, a, XCB_STACK_MODE_ABOVE, 2);
  change(b, 1, "_NET_WM_STATE_ABOVE");
  await([&]{ return state(b, "_NET_WM_STATE_ABOVE") && above(b, a); }, "restack ABOVE setup");
  restack(a, b, XCB_STACK_MODE_ABOVE); std::this_thread::sleep_for(100ms);
  check(above(b, a) && active(guard), "restack escaped ABOVE layer");
  change(b, 0, "_NET_WM_STATE_ABOVE");
  change(a, 1, "_NET_WM_STATE_BELOW");
  await([&]{ return state(a, "_NET_WM_STATE_BELOW") && above(b, a); }, "restack BELOW setup");
  restack(a, b, XCB_STACK_MODE_ABOVE); std::this_thread::sleep_for(100ms);
  check(above(b, a) && active(guard), "restack escaped BELOW layer");
  change(a, 0, "_NET_WM_STATE_BELOW");
  auto child = create(false); set(child, "WM_TRANSIENT_FOR", XCB_ATOM_WINDOW, a);
  xcb_map_window(connection, child); xcb_flush(connection);
  await([&]{ return above(child, a); }, "restack transient setup"); focus(guard);
  restack(a, child, XCB_STACK_MODE_ABOVE); std::this_thread::sleep_for(100ms);
  check(above(child, a) && active(guard), "restack placed parent above transient");

  auto fullscreen = create();
  auto monitors = [&]{ return property(fullscreen, "_NET_WM_FULLSCREEN_MONITORS"); };
  auto request_monitors = [&](std::uint32_t top, std::uint32_t bottom,
                              std::uint32_t left, std::uint32_t right) {
    message(fullscreen, "_NET_WM_FULLSCREEN_MONITORS", top, bottom, left, right, 2);
  };
  const std::vector<std::uint32_t> single{0, 0, 0, 0};
  request_monitors(0, 0, 0, 0);
  await([&]{ return monitors() == single; }, "valid fullscreen monitors not published");
  auto* published = xcb_get_property_reply(connection,
      xcb_get_property(connection, 0, fullscreen, atom("_NET_WM_FULLSCREEN_MONITORS"),
                       XCB_GET_PROPERTY_TYPE_ANY, 0, 8), nullptr);
  check(published && published->type == XCB_ATOM_CARDINAL && published->format == 32 &&
        published->value_len == 4 && published->bytes_after == 0,
        "fullscreen monitor property must contain exactly four CARDINAL/32 words");
  std::free(published);
  auto geometry = [&]{
    auto* g = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, fullscreen), nullptr);
    auto* position = xcb_translate_coordinates_reply(connection,
        xcb_translate_coordinates(connection, fullscreen, root, 0, 0), nullptr);
    bool ok = g && position && g->width == 1280 && g->height == 720 &&
              position->dst_x == 0 && position->dst_y == 0;
    std::free(g); std::free(position); return ok;
  };
  change(fullscreen, 1, "_NET_WM_STATE_FULLSCREEN");
  await(geometry, "fullscreen monitor request not used on fullscreen entry");
  request_monitors(0, 0, 0, 0); await(geometry, "live fullscreen monitor geometry");
  for (int index = 0; index < 4; ++index) {
    std::uint32_t invalid[4]{}; invalid[index] = 0xffffffffU;
    request_monitors(invalid[0], invalid[1], invalid[2], invalid[3]);
    std::this_thread::sleep_for(100ms);
    check(monitors() == single && geometry(), "invalid fullscreen monitor index accepted");
  }
  change(fullscreen, 0, "_NET_WM_STATE_FULLSCREEN");
  for (auto w : {a, b, guard, child, unmanaged, fullscreen}) xcb_destroy_window(connection, w);
  xcb_flush(connection);
  await([&]{ return property(root, "_NET_CLIENT_LIST").empty(); }, "protocol fixtures cleanup");
  std::cout << "Restack and fullscreen monitor protocol checks passed.\n";
}

void multiple_monitor_requests() {
  auto* reply = xcb_xinerama_query_screens_reply(connection,
      xcb_xinerama_query_screens(connection), nullptr);
  check(reply && xcb_xinerama_query_screens_screen_info_length(reply) == 2,
        "multi-monitor fixture requires two Xinerama screens");
  const auto* screens = xcb_xinerama_query_screens_screen_info(reply);
  check(screens[0].x_org == 0 && screens[0].y_org == 0 &&
        screens[0].width == 1280 && screens[0].height == 720 &&
        screens[1].x_org == 0 && screens[1].y_org == 0 &&
        screens[1].width == 800 && screens[1].height == 600,
        "unexpected Xinerama screen index ordering");
  std::free(reply);
  auto window = create();
  auto geometry = [&]{
    auto* g = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr);
    auto* p = xcb_translate_coordinates_reply(connection,
        xcb_translate_coordinates(connection, window, root, 0, 0), nullptr);
    check(g && p, "multi-monitor geometry query");
    std::vector<int> result{p->dst_x, p->dst_y, g->width, g->height};
    std::free(g); std::free(p); return result;
  };
  const auto original = geometry();
  auto property_value = [&]{ return property(window, "_NET_WM_FULLSCREEN_MONITORS"); };
  auto request = [&](std::uint32_t top, std::uint32_t bottom, std::uint32_t left,
                     std::uint32_t right, std::uint32_t source = 2) {
    message(window, "_NET_WM_FULLSCREEN_MONITORS", top, bottom, left, right, source);
  };
  request(0, 0, 0, 0);
  await([&]{ return property_value() == std::vector<std::uint32_t>{0, 0, 0, 0}; }, "multi-monitor initial property");
  check(geometry() == original, "monitor preference resized ordinary window");
  change(window, 1, "_NET_WM_STATE_FULLSCREEN");
  await([&]{ return geometry() == std::vector<int>{0, 0, 1280, 720}; }, "first monitor fullscreen geometry");
  request(1, 1, 1, 1);
  await([&]{ return geometry() == std::vector<int>{0, 0, 800, 600}; }, "live second monitor fullscreen geometry");
  request(0, 1, 0, 0);
  const std::vector<std::uint32_t> edges{0, 1, 0, 0};
  await([&]{ return property_value() == edges && geometry() == std::vector<int>{0, 0, 1280, 600}; },
        "fullscreen top bottom left right edge selection");
  request(1, 1, 1, 1, 3); std::this_thread::sleep_for(100ms);
  check(property_value() == edges && geometry() == std::vector<int>{0, 0, 1280, 600},
        "invalid fullscreen request source accepted");
  xcb_client_message_event_t malformed{};
  malformed.response_type = XCB_CLIENT_MESSAGE; malformed.format = 8;
  malformed.window = window; malformed.type = atom("_NET_WM_FULLSCREEN_MONITORS");
  malformed.data.data32[0] = malformed.data.data32[1] = 1;
  malformed.data.data32[2] = malformed.data.data32[3] = 1;
  malformed.data.data32[4] = 2;
  xcb_send_event(connection, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, reinterpret_cast<const char*>(&malformed));
  xcb_flush(connection); std::this_thread::sleep_for(100ms);
  check(property_value() == edges, "invalid fullscreen request format accepted");
  for (int index = 0; index < 4; ++index) {
    auto invalid = edges; invalid[index] = 2;
    request(invalid[0], invalid[1], invalid[2], invalid[3]);
    std::this_thread::sleep_for(100ms);
    check(property_value() == edges && geometry() == std::vector<int>{0, 0, 1280, 600},
          "out-of-range multi-monitor index accepted");
  }
  change(window, 0, "_NET_WM_STATE_FULLSCREEN");
  await([&]{ return geometry() == original; }, "fullscreen exit did not restore ordinary geometry");
  check(property_value() == edges, "fullscreen exit removed monitor preference");
  change(window, 1, "_NET_WM_STATE_FULLSCREEN");
  await([&]{ return geometry() == std::vector<int>{0, 0, 1280, 600}; }, "fullscreen reentry lost monitor preference");
  xcb_destroy_window(connection, window); xcb_flush(connection);
  std::cout << "Xinerama fullscreen monitor edge and live geometry checks passed.\n";
}

// PropertyNotify supplies the server clock, avoiding guesses from wall time.
xcb_timestamp_t server_time(xcb_window_t w) {
  const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_change_window_attributes(connection, w, XCB_CW_EVENT_MASK, &mask);
  while (auto* event = xcb_poll_for_event(connection)) std::free(event);
  set(w, "_MIDNIGHT_TEST_TIMESTAMP", XCB_ATOM_CARDINAL, 1);
  xcb_timestamp_t result = 0;
  await([&]{
    while (auto* event = xcb_poll_for_event(connection)) {
      if ((event->response_type & 0x7f) == XCB_PROPERTY_NOTIFY) {
        auto* p = reinterpret_cast<xcb_property_notify_event_t*>(event);
        if (p->window == w && p->atom == atom("_MIDNIGHT_TEST_TIMESTAMP")) result = p->time;
      }
      std::free(event);
    }
    return result != 0;
  }, "server timestamp");
  return result;
}
int main(int argc, char** argv) {
 try {
  connection = xcb_connect(nullptr, nullptr); check(!xcb_connection_has_error(connection), "X connection");
  root = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
  if (argc > 1 && std::string(argv[1]) == "--monitors-only") {
    multiple_monitor_requests(); xcb_disconnect(connection); return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--visual") {
    for (const auto* title : {"Attention example", "Foreground example"}) {
      auto window = create(false);
      if (std::string(title) == "Attention example")
        set(window, "_NET_WM_STATE", XCB_ATOM_ATOM, atom("_NET_WM_STATE_SHADED"));
      else {
        const std::uint32_t position[]{500, 240};
        xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, position);
      }
      xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
          XCB_ATOM_STRING, 8, std::char_traits<char>::length(title), title);
      const std::uint32_t background = 0xdddddd;
      xcb_change_window_attributes(connection, window, XCB_CW_BACK_PIXEL, &background);
      xcb_map_window(connection, window); xcb_flush(connection);
      await([&]{ auto list = property(root, "_NET_CLIENT_LIST");
        return std::find(list.begin(), list.end(), window) != list.end(); }, "visual fixture mapping");
    }
    std::this_thread::sleep_for(60s);
    xcb_disconnect(connection);
    return 0;
  }
  shade_requests();
  protocol_requests();
  auto a = create(), b = create(); focus(b);
  await([&]{ return state(b, "_NET_WM_STATE_FOCUSED") && !state(a, "_NET_WM_STATE_FOCUSED"); }, "FOCUSED publication");
  change(a, 1, "_NET_WM_STATE_FOCUSED"); std::this_thread::sleep_for(100ms);
  check(!state(a, "_NET_WM_STATE_FOCUSED") && active(b), "client forged FOCUSED");
  change(a, 1, "_NET_WM_STATE_ABOVE");
  await([&]{ return state(a, "_NET_WM_STATE_ABOVE") && above(a, b); }, "ABOVE stacking");
  focus(b); check(above(a, b), "focus escaped ABOVE layer");
  change(a, 1, "_NET_WM_STATE_BELOW");
  await([&]{ return state(a, "_NET_WM_STATE_BELOW") && !state(a, "_NET_WM_STATE_ABOVE") && above(b, a); }, "BELOW excludes ABOVE");
  focus(a); check(above(b, a), "focus escaped BELOW layer");
  change(a, 0, "_NET_WM_STATE_BELOW");
  change(a, 2, "_NET_WM_STATE_SKIP_PAGER", "_NET_WM_STATE_SKIP_PAGER");
  await([&]{ return state(a, "_NET_WM_STATE_SKIP_PAGER"); }, "duplicate toggle applied twice");
  change(a, 2, "_NET_WM_STATE_SKIP_PAGER");
  await([&]{ return !state(a, "_NET_WM_STATE_SKIP_PAGER"); }, "SKIP_PAGER toggle off");
  focus(b); change(a, 1, "_NET_WM_STATE_DEMANDS_ATTENTION");
  await([&]{ return state(a, "_NET_WM_STATE_DEMANDS_ATTENTION"); }, "attention request");
  check(active(b), "attention stole focus"); focus(a);
  await([&]{ return !state(a, "_NET_WM_STATE_DEMANDS_ATTENTION"); }, "focus did not clear attention");
  focus(b);
  const std::uint32_t urgency[] = {256, 0, 0, 0, 0, 0, 0, 0, 0};
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, a, atom("WM_HINTS"), atom("WM_HINTS"), 32, 9, urgency); xcb_flush(connection);
  await([&]{ return state(a, "_NET_WM_STATE_DEMANDS_ATTENTION"); }, "ICCCM urgency ignored"); focus(a);
  await([&]{ return !state(a, "_NET_WM_STATE_DEMANDS_ATTENTION"); }, "ICCCM attention not cleared");
  auto no_focus = create(false); set(no_focus, "_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, 0);
  focus(b); xcb_map_window(connection, no_focus); xcb_flush(connection);
  await([&]{ return !property(no_focus, "_NET_WM_ALLOWED_ACTIONS").empty(); }, "no-focus client adoption");
  check(active(b), "zero user time stole map focus");
  message(no_focus, "_NET_ACTIVE_WINDOW", 1, 0); std::this_thread::sleep_for(100ms);
  check(active(b), "zero application activation stole focus");
  await([&]{ return state(no_focus, "_NET_WM_STATE_DEMANDS_ATTENTION"); }, "denied activation did not request attention");
  focus(no_focus); message(no_focus, "WM_CHANGE_STATE", 3);
  await([&]{ return state(no_focus, "_NET_WM_STATE_HIDDEN") && !state(no_focus, "_NET_WM_STATE_FOCUSED"); }, "minimized FOCUSED state persisted");
  focus(b); message(no_focus, "_NET_ACTIVE_WINDOW", 1, 0); std::this_thread::sleep_for(100ms);
  check(active(b) && state(no_focus, "_NET_WM_STATE_HIDDEN"), "denied activation restored minimized window");
  message(no_focus, "_NET_WM_DESKTOP", 1);
  await([&]{ return property(no_focus, "_NET_WM_DESKTOP") == std::vector<std::uint32_t>{1}; }, "move minimized client");
  message(no_focus, "_NET_ACTIVE_WINDOW", 1, 0); std::this_thread::sleep_for(100ms);
  check(property(root, "_NET_CURRENT_DESKTOP") == std::vector<std::uint32_t>{0}, "denied activation switched workspace");
  message(root, "_NET_SHOWING_DESKTOP", 1);
  await([&]{ return property(root, "_NET_SHOWING_DESKTOP") == std::vector<std::uint32_t>{1}; }, "show desktop");
  message(no_focus, "_NET_ACTIVE_WINDOW", 1, 0); std::this_thread::sleep_for(100ms);
  check(property(root, "_NET_SHOWING_DESKTOP") == std::vector<std::uint32_t>{1}, "denied activation exited show desktop");
  message(root, "_NET_SHOWING_DESKTOP", 0); focus(b);
  // A valid user time from the active client establishes recent genuine input.
  auto old_time = server_time(b);
  std::this_thread::sleep_for(30ms);
  auto input_time = server_time(b);
  set(b, "_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, input_time);
  std::this_thread::sleep_for(100ms);
  for (auto source : {0U, 1U}) {
    message(a, "_NET_ACTIVE_WINDOW", source, old_time);
    std::this_thread::sleep_for(100ms);
    check(active(b), "stale application activation stole focus");
  }
  auto stale = create(false);
  set(stale, "_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, old_time);
  xcb_map_window(connection, stale); xcb_flush(connection);
  await([&]{ return !property(stale, "_NET_WM_ALLOWED_ACTIONS").empty(); }, "stale-time adoption");
  check(active(b), "stale map user time stole focus");
  auto fresh_time = server_time(b);
  message(a, "_NET_ACTIVE_WINDOW", 1, fresh_time);
  await([&]{ return active(a); }, "fresh application activation denied");
  focus(b);
  auto helper = create(false), timed = create(false);
  set(helper, "_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, 0);
  set(timed, "_NET_WM_USER_TIME_WINDOW", XCB_ATOM_WINDOW, helper);
  xcb_map_window(connection, timed); xcb_flush(connection);
  await([&]{ return !property(timed, "_NET_WM_ALLOWED_ACTIONS").empty(); }, "helper-time adoption");
  check(active(b), "helper zero user time ignored");
  // Changes on the helper remain observable after the client is managed.
  auto remap_timed = [&]{
    xcb_unmap_window(connection, timed); xcb_flush(connection);
    await([&]{ auto p = property(root, "_NET_CLIENT_LIST"); return std::find(p.begin(), p.end(), timed) == p.end(); }, "timed client withdrawal");
    xcb_map_window(connection, timed); xcb_flush(connection);
    await([&]{ return !property(timed, "_NET_WM_ALLOWED_ACTIONS").empty() &&
      [&]{ auto p = property(root, "_NET_CLIENT_LIST"); return std::find(p.begin(), p.end(), timed) != p.end(); }(); }, "timed client remapping");
  };
  set(helper, "_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, server_time(b));
  std::this_thread::sleep_for(50ms); remap_timed();
  await([&]{ return active(timed); }, "fresh helper update ignored on remap");
  focus(b); set(helper, "_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, 0);
  std::this_thread::sleep_for(50ms); remap_timed();
  check(active(b), "zero helper update ignored on remap");
  xcb_delete_property(connection, helper, atom("_NET_WM_USER_TIME")); xcb_flush(connection);
  std::this_thread::sleep_for(50ms); remap_timed();
  await([&]{ return active(timed); }, "deleted helper time retained zero hint");
  focus(b);
  set(timed, "_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, 0);
  set(timed, "_NET_WM_USER_TIME_WINDOW", XCB_ATOM_CARDINAL, helper);
  std::this_thread::sleep_for(50ms); remap_timed();
  check(active(b), "invalid helper WINDOW type bypassed own zero time");
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, timed, atom("_NET_WM_USER_TIME_WINDOW"), XCB_ATOM_WINDOW, 8, sizeof(helper), &helper);
  xcb_flush(connection); std::this_thread::sleep_for(50ms); remap_timed();
  check(active(b), "invalid helper WINDOW format bypassed own zero time");
  // A managed active client's helper also advances the last input timestamp.
  set(b, "_NET_WM_USER_TIME_WINDOW", XCB_ATOM_WINDOW, helper);
  auto helper_old = server_time(b);
  std::this_thread::sleep_for(30ms);
  set(helper, "_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, server_time(b));
  std::this_thread::sleep_for(100ms);
  message(a, "_NET_ACTIVE_WINDOW", 1, helper_old);
  std::this_thread::sleep_for(100ms);
  check(active(b), "active helper update failed to reject stale activation");
  // Real property notifications must reject future timestamps; SendEvent
  // notifications must never establish a trusted timestamp either.
  const auto valid_time = server_time(b);
  const auto future_time = valid_time + 60000U;
  set(helper, "_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, future_time);
  std::this_thread::sleep_for(100ms);
  message(a, "_NET_ACTIVE_WINDOW", 1, valid_time);
  await([&]{ return active(a); }, "real future user time poisoned activation baseline");
  focus(b);
  xcb_property_notify_event_t fake{};
  fake.response_type = XCB_PROPERTY_NOTIFY; fake.window = helper;
  fake.atom = atom("_NET_WM_USER_TIME"); fake.time = future_time;
  fake.state = XCB_PROPERTY_NEW_VALUE;
  xcb_send_event(connection, 0, helper, XCB_EVENT_MASK_PROPERTY_CHANGE, reinterpret_cast<const char*>(&fake));
  xcb_flush(connection); std::this_thread::sleep_for(100ms);
  message(a, "_NET_ACTIVE_WINDOW", 1, server_time(b));
  await([&]{ return active(a); }, "synthetic property event poisoned activation baseline");
  // The helper becomes a managed client, then loses its final external watcher.
  // It must retain its own WM_NORMAL_HINTS notifications after that transition.
  xcb_map_window(connection, helper); xcb_flush(connection);
  await([&]{ return !property(helper, "_NET_WM_ALLOWED_ACTIONS").empty(); }, "helper managed adoption");
  xcb_delete_property(connection, b, atom("_NET_WM_USER_TIME_WINDOW")); xcb_flush(connection);
  std::this_thread::sleep_for(100ms);
  std::uint32_t helper_hints[18]{};
  helper_hints[0] = (1U << 4) | (1U << 5);
  helper_hints[5] = helper_hints[7] = 260;
  helper_hints[6] = helper_hints[8] = 170;
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, helper, atom("WM_NORMAL_HINTS"), atom("WM_SIZE_HINTS"), 32, 18, helper_hints);
  xcb_flush(connection);
  await([&]{ return !has(helper, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_RESIZE"); }, "last helper detach removed managed property notifications");
  auto malformed = create(false);
  const auto forged_state = atom("_NET_WM_STATE_ABOVE");
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, malformed, atom("_NET_WM_STATE"), XCB_ATOM_ATOM, 8, sizeof(forged_state), &forged_state);
  set(malformed, "_NET_WM_USER_TIME", XCB_ATOM_CARDINAL, 0);
  xcb_map_window(connection, malformed); xcb_flush(connection);
  await([&]{ return !property(malformed, "_NET_WM_ALLOWED_ACTIONS").empty(); }, "malformed initial state adoption");
  check(!state(malformed, "_NET_WM_STATE_ABOVE"), "malformed initial ATOM format accepted");
  auto child = create(false);
  set(child, "WM_TRANSIENT_FOR", XCB_ATOM_WINDOW, a);
  set(child, "_NET_WM_STATE", XCB_ATOM_ATOM, atom("_NET_WM_STATE_MODAL"));
  xcb_map_window(connection, child); xcb_flush(connection);
  await([&]{ return above(child, a); }, "transient not above parent");
  message(a, "_NET_ACTIVE_WINDOW", 2);
  await([&]{ return active(child) && above(child, a); }, "parent activation did not preserve modal transient");
  focus(b); change(b, 1, "_NET_WM_STATE_FULLSCREEN");
  await([&]{ return state(b, "_NET_WM_STATE_FULLSCREEN") && above(b, a); }, "fullscreen layer");
  auto fullscreen_above_panel = [&]{
    auto* tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, b), nullptr);
    if (!tree) return false;
    const auto frame = tree->parent; std::free(tree);
    auto* root_tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, root), nullptr);
    if (!root_tree) return false;
    const auto* windows = xcb_query_tree_children(root_tree);
    int frame_index = -1, panel_index = -1;
    for (int i = 0; i < xcb_query_tree_children_length(root_tree); ++i) {
      if (windows[i] == frame) frame_index = i;
      auto* geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, windows[i]), nullptr);
      if (geometry && geometry->width == 1280 && geometry->height == 30) panel_index = i;
      std::free(geometry);
    }
    std::free(root_tree);
    return panel_index >= 0 && frame_index > panel_index;
  };
  await(fullscreen_above_panel, "panel escaped focused fullscreen layer");
  change(a, 1, "_NET_WM_STATE_ABOVE");
  await([&]{ return state(a, "_NET_WM_STATE_ABOVE") && active(b); }, "ABOVE update stole focus");
  await(fullscreen_above_panel, "task repaint raised panel over fullscreen");
  check(above(b, a), "ABOVE client escaped focused fullscreen layer");
  message(a, "_NET_ACTIVE_WINDOW", 2);
  await([&]{ return active(child) && above(a, b); }, "inactive fullscreen remained above ABOVE layer");
  change(b, 0, "_NET_WM_STATE_FULLSCREEN");
  change(a, 0, "_NET_WM_STATE_ABOVE");
  check(has(a, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_MOVE"), "MOVE action absent");
  check(has(a, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_CLOSE"), "CLOSE action absent");
  auto fixed = create(false);
  std::uint32_t hints[18]{}; hints[0] = (1U << 4) | (1U << 5); hints[5] = hints[7] = 260; hints[6] = hints[8] = 170;
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, fixed, atom("WM_NORMAL_HINTS"), atom("WM_SIZE_HINTS"), 32, 18, hints);
  xcb_map_window(connection, fixed); xcb_flush(connection);
  await([&]{ return !property(fixed, "_NET_WM_ALLOWED_ACTIONS").empty(); }, "fixed-size adoption");
  check(!has(fixed, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_RESIZE"), "fixed-size client advertised resize");
  hints[0] = 0;
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, fixed, atom("WM_NORMAL_HINTS"), atom("WM_SIZE_HINTS"), 32, 18, hints);
  xcb_flush(connection);
  await([&]{ return has(fixed, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_RESIZE"); }, "allowed resize not refreshed after hints changed");
  focus(fixed); change(fixed, 1, "_NET_WM_STATE_FULLSCREEN");
  await([&]{ return state(fixed, "_NET_WM_STATE_FULLSCREEN") &&
      !has(fixed, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_MOVE") &&
      !has(fixed, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_RESIZE"); }, "fullscreen allowed actions not refreshed");
  change(fixed, 0, "_NET_WM_STATE_FULLSCREEN");
  await([&]{ return has(fixed, "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_ACTION_RESIZE"); }, "fullscreen exit did not restore allowed resize");
  for (auto w : {a, b, no_focus, fixed, helper, timed, child, stale, malformed}) xcb_destroy_window(connection, w);
  xcb_flush(connection); xcb_disconnect(connection);
  std::cout << "EWMH stacking, attention, focus policy and allowed actions passed.\n";
 } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
