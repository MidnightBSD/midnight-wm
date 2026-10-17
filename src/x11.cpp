#include "x11.hpp"
#include "shell.hpp"

#include <cairo/cairo-xcb.h>
#include <pango/pangocairo.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/randr.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace midnight {
namespace {
constexpr int border = 5;
constexpr int title_height = 28;
constexpr int panel_height = 34;
constexpr std::uint16_t min_width = 80;
constexpr std::uint16_t min_height = 40;

void box(cairo_t* cr, double x, double y, double width, double height,
         double r, double g, double b) {
  cairo_rectangle(cr, x, y, width, height);
  cairo_set_source_rgb(cr, r, g, b);
  cairo_fill(cr);
}

void label(cairo_t* cr, const std::string& text, int x, int y, int width, bool bold = false) {
  auto* layout = pango_cairo_create_layout(cr);
  auto* desc = pango_font_description_from_string(bold ? "Sans Bold 10" : "Sans 10");
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_width(layout, width * PANGO_SCALE);
  pango_layout_set_text(layout, text.c_str(), -1);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

template <class T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
} // namespace

X11WindowManager::X11WindowManager(xcb_connection_t* connection, xcb_screen_t* screen, Shell& shell)
  : connection_(connection), screen_(screen), shell_(shell),
    screen_width_(screen->width_in_pixels), screen_height_(screen->height_in_pixels) {}

X11WindowManager::~X11WindowManager() {
  for (auto& [client, frame] : frames_) {
    xcb_reparent_window(connection_, client, screen_->root, frame.x, frame.y);
    xcb_change_save_set(connection_, XCB_SET_MODE_DELETE, client);
    xcb_destroy_window(connection_, frame.frame);
  }
  if (support_window_) xcb_destroy_window(connection_, support_window_);
  xcb_delete_property(connection_, screen_->root, atom("_NET_SUPPORTING_WM_CHECK"));
  xcb_delete_property(connection_, screen_->root, atom("_NET_ACTIVE_WINDOW"));
  xcb_delete_property(connection_, screen_->root, atom("_NET_CLIENT_LIST"));
  if (root_cursor_) {
    const std::uint32_t none = XCB_NONE;
    xcb_change_window_attributes(connection_, screen_->root, XCB_CW_CURSOR, &none);
    xcb_free_cursor(connection_, root_cursor_);
  }
  xcb_flush(connection_);
}

bool X11WindowManager::install_root_cursor() {
  // A bitmap cursor is available even when a cursor theme or cursor font is not.
  constexpr std::array<const char*, 16> arrow{
    "B...............", "BB..............", "BWB.............", "BWWB............",
    "BWWWB...........", "BWWWWB..........", "BWWWWWB.........", "BWWWWWWB........",
    "BWWWWWWWB.......", "BWWWWWWWWB......", "BWWWWWBBBB......", "BWWBWWB.........",
    "BW.BWWB.........", "B..BWWB.........", "...BWWB.........", "...BBBB........."
  };
  const auto source = xcb_generate_id(connection_);
  const auto mask = xcb_generate_id(connection_);
  const auto gc = xcb_generate_id(connection_);
  xcb_create_pixmap(connection_, 1, source, screen_->root, 16, 16);
  xcb_create_pixmap(connection_, 1, mask, screen_->root, 16, 16);
  const std::uint32_t black = 0;
  xcb_create_gc(connection_, gc, source, XCB_GC_FOREGROUND, &black);
  const xcb_rectangle_t bounds{0, 0, 16, 16};
  xcb_poly_fill_rectangle(connection_, source, gc, 1, &bounds);
  xcb_poly_fill_rectangle(connection_, mask, gc, 1, &bounds);
  const std::uint32_t white = 1;
  xcb_change_gc(connection_, gc, XCB_GC_FOREGROUND, &white);
  std::vector<xcb_point_t> shape;
  std::vector<xcb_point_t> visible;
  for (std::int16_t y = 0; y < 16; ++y)
    for (std::int16_t x = 0; x < 16; ++x) {
      const char pixel = arrow[y][x];
      if (pixel != '.') visible.push_back({x, y});
      if (pixel == 'B') shape.push_back({x, y});
    }
  xcb_poly_point(connection_, XCB_COORD_MODE_ORIGIN, source, gc, shape.size(), shape.data());
  xcb_poly_point(connection_, XCB_COORD_MODE_ORIGIN, mask, gc, visible.size(), visible.data());
  xcb_free_gc(connection_, gc);

  root_cursor_ = xcb_generate_id(connection_);
  const auto created = xcb_create_cursor_checked(connection_, root_cursor_, source, mask,
                                                  0, 0, 0, 0xffff, 0xffff, 0xffff, 0, 0);
  xcb_free_pixmap(connection_, source);
  xcb_free_pixmap(connection_, mask);
  if (auto* error = xcb_request_check(connection_, created)) {
    std::cerr << "Could not create pointer cursor (X error " << int(error->error_code) << ")\n";
    std::free(error);
    root_cursor_ = XCB_NONE;
    return false;
  }
  const std::uint32_t cursor = root_cursor_;
  const auto applied = xcb_change_window_attributes_checked(connection_, screen_->root, XCB_CW_CURSOR, &cursor);
  if (auto* error = xcb_request_check(connection_, applied)) {
    std::cerr << "Could not set pointer cursor (X error " << int(error->error_code) << ")\n";
    std::free(error);
    xcb_free_cursor(connection_, root_cursor_);
    root_cursor_ = XCB_NONE;
    return false;
  }
  return true;
}

xcb_atom_t X11WindowManager::atom(const std::string& name) {
  if (auto found = atoms_.find(name); found != atoms_.end()) return found->second;
  auto cookie = xcb_intern_atom(connection_, 0, name.size(), name.c_str());
  Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(connection_, cookie, nullptr), std::free);
  const auto value = reply ? reply->atom : XCB_ATOM_NONE;
  atoms_[name] = value;
  return value;
}

xcb_visualtype_t* X11WindowManager::root_visual() const {
  auto depth = xcb_screen_allowed_depths_iterator(screen_);
  for (; depth.rem; xcb_depth_next(&depth)) {
    auto visual = xcb_depth_visuals_iterator(depth.data);
    for (; visual.rem; xcb_visualtype_next(&visual))
      if (visual.data->visual_id == screen_->root_visual) return visual.data;
  }
  return nullptr;
}

X11WindowManager::Monitor X11WindowManager::monitor_for(const Frame& frame) const {
  const Monitor fallback{0, 0, screen_width_, screen_height_};
  const auto* extension = xcb_get_extension_data(connection_, &xcb_randr_id);
  if (!extension || !extension->present) return fallback;
  auto cookie = xcb_randr_get_monitors(connection_, screen_->root, 1);
  Reply<xcb_randr_get_monitors_reply_t> reply(xcb_randr_get_monitors_reply(connection_, cookie, nullptr), std::free);
  if (!reply) return fallback;
  const int cx = frame.x + frame.width / 2;
  const int cy = frame.y + frame.height / 2;
  auto monitors = xcb_randr_get_monitors_monitors_iterator(reply.get());
  Monitor first = fallback;
  bool found = false;
  for (; monitors.rem; xcb_randr_monitor_info_next(&monitors)) {
    const auto* info = monitors.data;
    if (!info || !info->width || !info->height) continue;
    const Monitor monitor{info->x, info->y, info->width, info->height};
    if (!found) { first = monitor; found = true; }
    if (cx >= monitor.x && cx < monitor.x + monitor.width &&
        cy >= monitor.y && cy < monitor.y + monitor.height) return monitor;
  }
  return first;
}

bool X11WindowManager::initialize() {
  const auto mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                    XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  auto cookie = xcb_change_window_attributes_checked(connection_, screen_->root, XCB_CW_EVENT_MASK, &mask);
  if (auto* error = xcb_request_check(connection_, cookie)) {
    std::cerr << "Cannot manage this display (another window manager is running, X error "
              << int(error->error_code) << ")\n";
    std::free(error);
    return false;
  }
  if (!install_root_cursor()) return false;
  set_root_properties();
  install_key_grabs();
  scan_existing();
  xcb_flush(connection_);
  return true;
}

void X11WindowManager::set_root_properties() {
  support_window_ = xcb_generate_id(connection_);
  xcb_create_window(connection_, XCB_COPY_FROM_PARENT, support_window_, screen_->root,
                    0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                    XCB_COPY_FROM_PARENT, 0, nullptr);
  const auto check = atom("_NET_SUPPORTING_WM_CHECK");
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, check,
                      XCB_ATOM_WINDOW, 32, 1, &support_window_);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, support_window_, check,
                      XCB_ATOM_WINDOW, 32, 1, &support_window_);
  const auto utf8 = atom("UTF8_STRING");
  constexpr char name[] = "Midnight Workspace";
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, support_window_, atom("_NET_WM_NAME"),
                      utf8, 8, sizeof(name) - 1, name);
  std::vector<xcb_atom_t> supported;
  for (const auto* key : {"_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK", "_NET_CLIENT_LIST",
                          "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW", "_NET_WM_NAME",
                          "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN",
                          "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ",
                          "_NET_FRAME_EXTENTS",
                          "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP", "_NET_WORKAREA",
                          "_NET_DESKTOP_GEOMETRY", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK"})
    supported.push_back(atom(key));
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_SUPPORTED"),
                      XCB_ATOM_ATOM, 32, supported.size(), supported.data());
  const std::uint32_t one = 1, zero = 0;
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_NUMBER_OF_DESKTOPS"),
                      XCB_ATOM_CARDINAL, 32, 1, &one);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_CURRENT_DESKTOP"),
                      XCB_ATOM_CARDINAL, 32, 1, &zero);
  update_geometry_properties();
}

void X11WindowManager::update_geometry_properties() {
  const std::uint32_t geometry[] = {screen_width_, screen_height_};
  const std::uint32_t workarea[] = {0, panel_height, screen_width_,
                                   std::uint32_t(screen_height_ > panel_height ? screen_height_ - panel_height : 0)};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_DESKTOP_GEOMETRY"),
                      XCB_ATOM_CARDINAL, 32, 2, geometry);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_WORKAREA"),
                      XCB_ATOM_CARDINAL, 32, 4, workarea);
}

void X11WindowManager::scan_existing() {
  auto cookie = xcb_query_tree(connection_, screen_->root);
  Reply<xcb_query_tree_reply_t> reply(xcb_query_tree_reply(connection_, cookie, nullptr), std::free);
  if (!reply) return;
  auto* children = xcb_query_tree_children(reply.get());
  for (int i = 0; i < xcb_query_tree_children_length(reply.get()); ++i) manage(children[i]);
}

void X11WindowManager::manage(xcb_window_t window) {
  if (!window || window == support_window_ || frames_.contains(window) || shell_.owns(window)) return;
  Reply<xcb_get_window_attributes_reply_t> attr(
      xcb_get_window_attributes_reply(connection_, xcb_get_window_attributes(connection_, window), nullptr),
      std::free);
  if (!attr || attr->override_redirect || attr->_class == XCB_WINDOW_CLASS_INPUT_ONLY) return;
  auto type_cookie = xcb_get_property(connection_, 0, window, atom("_NET_WM_WINDOW_TYPE"),
                                      XCB_ATOM_ATOM, 0, 8);
  Reply<xcb_get_property_reply_t> types(xcb_get_property_reply(connection_, type_cookie, nullptr), std::free);
  if (types) {
    auto* values = static_cast<xcb_atom_t*>(xcb_get_property_value(types.get()));
    for (int i = 0; i < xcb_get_property_value_length(types.get()) / 4; ++i)
      if (values[i] == atom("_NET_WM_WINDOW_TYPE_DOCK") ||
          values[i] == atom("_NET_WM_WINDOW_TYPE_DESKTOP")) return;
  }
  Reply<xcb_get_geometry_reply_t> geometry(
      xcb_get_geometry_reply(connection_, xcb_get_geometry(connection_, window), nullptr), std::free);
  if (!geometry) return;
  Frame frame;
  frame.client = window;
  frame.frame = xcb_generate_id(connection_);
  frame.x = geometry->x;
  frame.y = std::max<int>(geometry->y, panel_height);
  frame.width = std::max<std::uint16_t>(geometry->width, min_width);
  frame.height = std::max<std::uint16_t>(geometry->height, min_height);
  frame.ignore_unmap = attr->map_state != XCB_MAP_STATE_UNMAPPED ? 1 : 0;
  frame.mapped = attr->map_state != XCB_MAP_STATE_UNMAPPED;
  const std::uint32_t mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
      XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  const std::uint32_t values[] = {screen_->white_pixel, mask};
  xcb_create_window(connection_, screen_->root_depth, frame.frame, screen_->root,
                    frame.x, frame.y, frame.width + 2 * border,
                    frame.height + title_height + border, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen_->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
  xcb_change_save_set(connection_, XCB_SET_MODE_INSERT, window);
  xcb_reparent_window(connection_, window, frame.frame, border, title_height);
  const std::uint32_t client_events = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  xcb_change_window_attributes(connection_, window, XCB_CW_EVENT_MASK, &client_events);
  const std::uint32_t extents[] = {border, border, title_height, border};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window, atom("_NET_FRAME_EXTENTS"),
                      XCB_ATOM_CARDINAL, 32, 4, extents);
  xcb_grab_button(connection_, 0, window, XCB_EVENT_MASK_BUTTON_PRESS,
                  XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE,
                  XCB_CURSOR_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);
  update_title(frame);
  frame_to_client_[frame.frame] = window;
  frames_[window] = frame;
  if (frame.mapped) map(window);
  update_client_list();
  update_tasks();
}

void X11WindowManager::unmanage(xcb_window_t client, bool destroyed) {
  auto it = frames_.find(client);
  if (it == frames_.end()) return;
  auto frame = it->second;
  if (!destroyed) {
    xcb_reparent_window(connection_, client, screen_->root, frame.x + border, frame.y + title_height);
    xcb_change_save_set(connection_, XCB_SET_MODE_DELETE, client);
  }
  xcb_destroy_window(connection_, frame.frame);
  frame_to_client_.erase(frame.frame);
  frames_.erase(it);
  if (active_ == client) active_ = 0;
  update_client_list();
  update_tasks();
}

void X11WindowManager::map(xcb_window_t client) {
  auto it = frames_.find(client);
  if (it == frames_.end()) return;
  it->second.mapped = true;
  xcb_map_window(connection_, it->second.frame);
  xcb_map_window(connection_, client);
  constexpr std::uint32_t normal[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_WINDOW_NONE};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("WM_STATE"), atom("WM_STATE"), 32, 2, normal);
  focus(client);
}

void X11WindowManager::focus(xcb_window_t client) {
  auto it = frames_.find(client);
  if (it == frames_.end() || !it->second.mapped) return;
  auto old = active_;
  active_ = client;
  xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_POINTER_ROOT, client, XCB_CURRENT_TIME);
  const std::uint32_t stack[] = {XCB_STACK_MODE_ABOVE};
  xcb_configure_window(connection_, it->second.frame, XCB_CONFIG_WINDOW_STACK_MODE, stack);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_ACTIVE_WINDOW"),
                      XCB_ATOM_WINDOW, 32, 1, &client);
  if (auto previous = frames_.find(old); previous != frames_.end()) draw_frame(previous->second);
  draw_frame(it->second);
  update_tasks();
  if (it->second.fullscreen)
    xcb_configure_window(connection_, it->second.frame, XCB_CONFIG_WINDOW_STACK_MODE, stack);
}

void X11WindowManager::update_client_list() {
  std::vector<xcb_window_t> clients;
  for (auto& [client, frame] : frames_) clients.push_back(client);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_CLIENT_LIST"),
                      XCB_ATOM_WINDOW, 32, clients.size(), clients.data());
}

void X11WindowManager::update_tasks() {
  std::vector<Task> tasks;
  for (auto& [client, frame] : frames_) tasks.push_back({client, frame.title, client == active_});
  shell_.set_tasks(std::move(tasks));
}

void X11WindowManager::update_title(Frame& frame) {
  auto read = [&](xcb_atom_t property, xcb_atom_t type) -> std::string {
    auto cookie = xcb_get_property(connection_, 0, frame.client, property, type, 0, 256);
    Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection_, cookie, nullptr), std::free);
    if (!reply || !xcb_get_property_value_length(reply.get())) return {};
    return {static_cast<const char*>(xcb_get_property_value(reply.get())),
            static_cast<std::size_t>(xcb_get_property_value_length(reply.get()))};
  };
  frame.title = read(atom("_NET_WM_NAME"), atom("UTF8_STRING"));
  if (frame.title.empty()) frame.title = read(XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY);
  if (frame.title.empty()) frame.title = "Untitled";
}

void X11WindowManager::draw_frame(Frame& frame) {
  auto* visual = root_visual();
  if (!visual) return;
  const int width = frame.width + (frame.fullscreen ? 0 : 2 * border);
  const int height = frame.height + (frame.fullscreen ? 0 : title_height + border);
  auto* surface = cairo_xcb_surface_create(connection_, frame.frame, visual, width, height);
  auto* cr = cairo_create(surface);
  box(cr, 0, 0, width, height, .78, .79, .80);
  box(cr, 0, 0, width, 2, .98, .98, .98);
  box(cr, 0, 0, 2, height, .98, .98, .98);
  box(cr, width - 2, 0, 2, height, .25, .27, .29);
  box(cr, 0, height - 2, width, 2, .25, .27, .29);
  if (frame.fullscreen) { cairo_destroy(cr); cairo_surface_destroy(surface); return; }
  const bool selected = active_ == frame.client;
  const auto& accent = shell_.accent_color();
  box(cr, border, border, width - 2 * border, title_height - border,
      selected ? accent[0] : accent[0] * .65 + .18,
      selected ? accent[1] : accent[1] * .65 + .18,
      selected ? accent[2] : accent[2] * .65 + .18);
  cairo_set_source_rgb(cr, 1, 1, 1);
  label(cr, frame.title, border + 9, border + 4, std::max(1, width - 124), true);
  const int button_y = 7;
  for (int i = 0; i < 3; ++i) {
    const int x = width - 74 + i * 23;
    box(cr, x, button_y, 20, 17, .88, .88, .86);
    box(cr, x, button_y, 20, 1, 1, 1, 1);
    box(cr, x + 19, button_y, 1, 17, .2, .2, .2);
    box(cr, x, button_y + 16, 20, 1, .2, .2, .2);
    cairo_set_source_rgb(cr, .12, .12, .12);
    label(cr, i == 0 ? "_" : i == 1 ? "□" : "×", x + 5, button_y - 2, 16, true);
  }
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  cairo_surface_destroy(surface);
}

void X11WindowManager::resize_frame(Frame& frame) {
  const std::uint32_t position[] = {static_cast<std::uint32_t>(frame.x), static_cast<std::uint32_t>(frame.y),
                                    static_cast<std::uint32_t>(frame.width + (frame.fullscreen ? 0 : 2 * border)),
                                    static_cast<std::uint32_t>(frame.height + (frame.fullscreen ? 0 : title_height + border))};
  xcb_configure_window(connection_, frame.frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, position);
  const std::uint32_t dimensions[] = {static_cast<std::uint32_t>(frame.fullscreen ? 0 : border),
                                      static_cast<std::uint32_t>(frame.fullscreen ? 0 : title_height),
                                      frame.width, frame.height};
  xcb_configure_window(connection_, frame.client, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, dimensions);
  draw_frame(frame);
}

void X11WindowManager::close(xcb_window_t client) {
  auto cookie = xcb_get_property(connection_, 0, client, atom("WM_PROTOCOLS"), XCB_ATOM_ATOM, 0, 32);
  Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection_, cookie, nullptr), std::free);
  const auto deletion = atom("WM_DELETE_WINDOW");
  bool supports_delete = false;
  if (reply) {
    auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply.get()));
    for (int i = 0; i < xcb_get_property_value_length(reply.get()) / 4; ++i)
      if (atoms[i] == deletion) supports_delete = true;
  }
  if (supports_delete) {
    xcb_client_message_event_t event{};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = client;
    event.type = atom("WM_PROTOCOLS");
    event.data.data32[0] = deletion;
    event.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(connection_, 0, client, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&event));
  } else xcb_kill_client(connection_, client);
}

void X11WindowManager::toggle_maximize(xcb_window_t client) {
  auto it = frames_.find(client);
  if (it == frames_.end()) return;
  auto& frame = it->second;
  if (!frame.maximized) {
    frame.old_x = frame.x; frame.old_y = frame.y;
    frame.old_width = frame.width; frame.old_height = frame.height;
    const auto monitor = monitor_for(frame);
    const int top = monitor.y == 0 ? panel_height : monitor.y;
    frame.x = monitor.x; frame.y = top;
    frame.width = std::max<int>(min_width, monitor.width - 2 * border);
    frame.height = std::max<int>(min_height, monitor.height - (top - monitor.y) - title_height - border);
  } else {
    frame.x = frame.old_x; frame.y = frame.old_y;
    frame.width = frame.old_width; frame.height = frame.old_height;
  }
  frame.maximized = !frame.maximized;
  std::vector<xcb_atom_t> states;
  if (frame.maximized) states = {atom("_NET_WM_STATE_MAXIMIZED_VERT"), atom("_NET_WM_STATE_MAXIMIZED_HORZ")};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("_NET_WM_STATE"),
                      XCB_ATOM_ATOM, 32, states.size(), states.data());
  resize_frame(frame);
}

void X11WindowManager::set_fullscreen(xcb_window_t client, bool enabled) {
  auto it = frames_.find(client);
  if (it == frames_.end() || it->second.fullscreen == enabled) return;
  auto& frame = it->second;
  if (enabled) {
    frame.old_x = frame.x; frame.old_y = frame.y;
    frame.old_width = frame.width; frame.old_height = frame.height;
    const auto monitor = monitor_for(frame);
    frame.x = monitor.x; frame.y = monitor.y;
    frame.width = monitor.width; frame.height = monitor.height;
  } else {
    frame.x = frame.old_x; frame.y = frame.old_y;
    frame.width = frame.old_width; frame.height = frame.old_height;
  }
  frame.fullscreen = enabled;
  const auto state = atom("_NET_WM_STATE_FULLSCREEN");
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("_NET_WM_STATE"),
                      XCB_ATOM_ATOM, 32, enabled ? 1 : 0, enabled ? &state : nullptr);
  resize_frame(frame);
  if (enabled) {
    const std::uint32_t above[] = {XCB_STACK_MODE_ABOVE};
    xcb_configure_window(connection_, frame.frame, XCB_CONFIG_WINDOW_STACK_MODE, above);
  } else update_tasks();
}

void X11WindowManager::minimize(xcb_window_t client) {
  auto it = frames_.find(client);
  if (it == frames_.end()) return;
  xcb_unmap_window(connection_, it->second.frame);
  it->second.mapped = false;
  constexpr std::uint32_t iconic[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_WINDOW_NONE};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("WM_STATE"), atom("WM_STATE"), 32, 2, iconic);
  if (active_ == client) active_ = 0;
  update_tasks();
}

void X11WindowManager::restore(xcb_window_t client) { map(client); }

xcb_window_t X11WindowManager::client_for(xcb_window_t window) const {
  if (frames_.contains(window)) return window;
  if (auto found = frame_to_client_.find(window); found != frame_to_client_.end()) return found->second;
  return 0;
}

void X11WindowManager::install_key_grabs() {
  auto* symbols = xcb_key_symbols_alloc(connection_);
  if (!symbols) return;
  auto grab = [&](xcb_keysym_t key, std::uint16_t modifiers) {
    auto* codes = xcb_key_symbols_get_keycode(symbols, key);
    if (!codes) return;
    for (auto* code = codes; *code; ++code)
      for (std::uint16_t ignored : {std::uint16_t(0), std::uint16_t(XCB_MOD_MASK_LOCK),
                                    std::uint16_t(XCB_MOD_MASK_2),
                                    std::uint16_t(XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK)})
        xcb_grab_key(connection_, 1, screen_->root, modifiers | ignored, *code,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    std::free(codes);
  };
  grab(XK_Tab, XCB_MOD_MASK_1);
  grab(XK_F4, XCB_MOD_MASK_1);
  xcb_key_symbols_free(symbols);
}

void X11WindowManager::cycle_focus() {
  if (frames_.empty()) return;
  auto it = frames_.upper_bound(active_);
  if (it == frames_.end()) it = frames_.begin();
  auto first = it;
  while (!it->second.mapped) {
    if (++it == frames_.end()) it = frames_.begin();
    if (it == first) return;
  }
  focus(it->first);
}

void X11WindowManager::update_screen_size(std::uint16_t width, std::uint16_t height) {
  screen_width_ = width; screen_height_ = height;
  update_geometry_properties();
  shell_.set_screen_size(width, height);
  for (auto& [client, frame] : frames_) {
    if (frame.fullscreen) {
      const auto monitor = monitor_for(frame);
      frame.x = monitor.x; frame.y = monitor.y;
      frame.width = monitor.width; frame.height = monitor.height;
      resize_frame(frame);
    } else if (frame.maximized) {
      const auto monitor = monitor_for(frame);
      const int top = monitor.y == 0 ? panel_height : monitor.y;
      frame.x = monitor.x; frame.y = top;
      frame.width = std::max<int>(min_width, monitor.width - 2 * border);
      frame.height = std::max<int>(min_height, monitor.height - (top - monitor.y) - title_height - border);
      resize_frame(frame);
    }
  }
}

void X11WindowManager::handle(xcb_generic_event_t* event) {
  const auto type = event->response_type & ~0x80;
  switch (type) {
  case XCB_MAP_REQUEST: {
    auto* e = reinterpret_cast<xcb_map_request_event_t*>(event);
    if (shell_.owns(e->window)) break;
    if (!frames_.contains(e->window)) manage(e->window);
    if (frames_.contains(e->window)) map(e->window);
    else xcb_map_window(connection_, e->window);
    break;
  }
  case XCB_CONFIGURE_REQUEST: {
    auto* e = reinterpret_cast<xcb_configure_request_event_t*>(event);
    auto it = frames_.find(e->window);
    if (it == frames_.end()) {
      std::vector<std::uint32_t> values;
      if (e->value_mask & XCB_CONFIG_WINDOW_X) values.push_back(static_cast<std::uint32_t>(e->x));
      if (e->value_mask & XCB_CONFIG_WINDOW_Y) values.push_back(static_cast<std::uint32_t>(e->y));
      if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) values.push_back(e->width);
      if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) values.push_back(e->height);
      if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values.push_back(e->border_width);
      if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING) values.push_back(e->sibling);
      if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) values.push_back(e->stack_mode);
      xcb_configure_window(connection_, e->window, e->value_mask, values.data());
      break;
    }
    auto& f = it->second;
    if (!f.maximized && !f.fullscreen) {
      if (e->value_mask & XCB_CONFIG_WINDOW_X) f.x = e->x;
      if (e->value_mask & XCB_CONFIG_WINDOW_Y) f.y = e->y;
      if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) f.width = std::max(e->width, min_width);
      if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) f.height = std::max(e->height, min_height);
      resize_frame(f);
    }
    xcb_configure_notify_event_t notify{};
    notify.response_type = XCB_CONFIGURE_NOTIFY;
    notify.event = f.client; notify.window = f.client;
    notify.x = f.x + border; notify.y = f.y + title_height;
    notify.width = f.width; notify.height = f.height;
    xcb_send_event(connection_, 0, f.client, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&notify));
    break;
  }
  case XCB_UNMAP_NOTIFY: {
    auto* e = reinterpret_cast<xcb_unmap_notify_event_t*>(event);
    if (auto it = frames_.find(e->window); it != frames_.end()) {
      if (it->second.ignore_unmap > 0) --it->second.ignore_unmap;
      else if (it->second.mapped) unmanage(e->window, false);
    }
    break;
  }
  case XCB_DESTROY_NOTIFY: {
    auto* e = reinterpret_cast<xcb_destroy_notify_event_t*>(event);
    if (frames_.contains(e->window)) unmanage(e->window, true);
    break;
  }
  case XCB_EXPOSE: {
    auto* e = reinterpret_cast<xcb_expose_event_t*>(event);
    if (auto found = frame_to_client_.find(e->window); found != frame_to_client_.end())
      draw_frame(frames_.at(found->second));
    break;
  }
  case XCB_PROPERTY_NOTIFY: {
    auto* e = reinterpret_cast<xcb_property_notify_event_t*>(event);
    if (auto it = frames_.find(e->window); it != frames_.end() &&
        (e->atom == atom("_NET_WM_NAME") || e->atom == XCB_ATOM_WM_NAME)) {
      update_title(it->second); draw_frame(it->second); update_tasks();
    }
    break;
  }
  case XCB_BUTTON_PRESS: {
    auto* e = reinterpret_cast<xcb_button_press_event_t*>(event);
    const auto client = client_for(e->event);
    if (!client) break;
    if (e->event == client) {
      focus(client);
      xcb_allow_events(connection_, XCB_ALLOW_REPLAY_POINTER, e->time);
      break;
    }
    auto& f = frames_.at(client);
    focus(client);
    if (e->detail != 1) break;
    const int x = e->event_x, y = e->event_y;
    if (y < title_height && x > int(f.width + 2 * border) - 74) {
      const int button = (x - (int(f.width + 2 * border) - 74)) / 23;
      if (button == 0) minimize(client);
      else if (button == 1) toggle_maximize(client);
      else close(client);
      break;
    }
    drag_mode_ = y < title_height ? DragMode::move : DragMode::resize;
    drag_client_ = client;
    drag_root_x_ = e->root_x; drag_root_y_ = e->root_y;
    drag_x_ = f.x; drag_y_ = f.y; drag_width_ = f.width; drag_height_ = f.height;
    xcb_grab_pointer(connection_, 0, f.frame, XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE, XCB_CURSOR_NONE, XCB_CURRENT_TIME);
    break;
  }
  case XCB_MOTION_NOTIFY: {
    auto* e = reinterpret_cast<xcb_motion_notify_event_t*>(event);
    if (drag_mode_ == DragMode::none || !frames_.contains(drag_client_)) break;
    auto& f = frames_.at(drag_client_);
    if (drag_mode_ == DragMode::move) {
      f.x = drag_x_ + e->root_x - drag_root_x_;
      f.y = drag_y_ + e->root_y - drag_root_y_;
    } else {
      f.width = std::max<int>(min_width, int(drag_width_) + e->root_x - drag_root_x_);
      f.height = std::max<int>(min_height, int(drag_height_) + e->root_y - drag_root_y_);
    }
    resize_frame(f);
    break;
  }
  case XCB_BUTTON_RELEASE:
    drag_mode_ = DragMode::none; drag_client_ = 0;
    xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
    break;
  case XCB_KEY_PRESS: {
    auto* e = reinterpret_cast<xcb_key_press_event_t*>(event);
    if (!(e->state & XCB_MOD_MASK_1)) break;
    auto* symbols = xcb_key_symbols_alloc(connection_);
    if (!symbols) break;
    auto key = xcb_key_symbols_get_keysym(symbols, e->detail, 0);
    xcb_key_symbols_free(symbols);
    if (key == XK_Tab) cycle_focus();
    else if (key == XK_F4 && active_) close(active_);
    break;
  }
  case XCB_CLIENT_MESSAGE: {
    auto* e = reinterpret_cast<xcb_client_message_event_t*>(event);
    if (e->type == atom("_NET_ACTIVE_WINDOW")) {
      if (frames_.contains(e->window)) { restore(e->window); focus(e->window); }
    } else if (e->type == atom("_NET_CLOSE_WINDOW")) close(e->window);
    else if (e->type == atom("_NET_WM_STATE")) {
      auto state = e->data.data32[1];
      if (state == atom("_NET_WM_STATE_FULLSCREEN")) {
        auto it = frames_.find(e->window);
        if (it != frames_.end()) set_fullscreen(e->window, e->data.data32[0] == 1 ||
            (e->data.data32[0] == 2 && !it->second.fullscreen));
      } else if (state == atom("_NET_WM_STATE_MAXIMIZED_VERT") ||
                 state == atom("_NET_WM_STATE_MAXIMIZED_HORZ")) toggle_maximize(e->window);
    }
    break;
  }
  case XCB_CONFIGURE_NOTIFY: {
    auto* e = reinterpret_cast<xcb_configure_notify_event_t*>(event);
    if (e->window == screen_->root && (e->width != screen_width_ || e->height != screen_height_))
      update_screen_size(e->width, e->height);
    break;
  }
  default: break;
  }
}
} // namespace midnight
