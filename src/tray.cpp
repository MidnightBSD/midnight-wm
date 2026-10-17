#include "tray.hpp"
#include "shell.hpp"

#include <cairo/cairo-xcb.h>

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace midnight {
namespace {
constexpr int icon_size = 22;
constexpr int icon_step = 26;
constexpr int tray_padding = 4;
constexpr int tray_y = 4;

template <class T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
} // namespace

X11SystemTray::X11SystemTray(xcb_connection_t* connection, xcb_screen_t* screen,
                             int screen_number, Shell& shell)
  : connection_(connection), screen_(screen), screen_number_(screen_number), shell_(shell) {}

X11SystemTray::~X11SystemTray() { release(); }

xcb_atom_t X11SystemTray::intern(const std::string& name) const {
  auto cookie = xcb_intern_atom(connection_, 0, name.size(), name.c_str());
  Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(connection_, cookie, nullptr), std::free);
  return reply ? reply->atom : XCB_ATOM_NONE;
}

bool X11SystemTray::initialize() {
  if (!shell_.panel_window()) return false;
  selection_ = intern("_NET_SYSTEM_TRAY_S" + std::to_string(screen_number_));
  opcode_ = intern("_NET_SYSTEM_TRAY_OPCODE");
  xembed_ = intern("_XEMBED");
  xembed_info_ = intern("_XEMBED_INFO");
  if (!selection_ || !opcode_ || !xembed_ || !xembed_info_) return false;
  auto owner_cookie = xcb_get_selection_owner(connection_, selection_);
  Reply<xcb_get_selection_owner_reply_t> previous(
      xcb_get_selection_owner_reply(connection_, owner_cookie, nullptr), std::free);
  if (!previous || previous->owner != XCB_WINDOW_NONE) return false;

  owner_ = xcb_generate_id(connection_);
  xcb_create_window(connection_, XCB_COPY_FROM_PARENT, owner_, screen_->root,
                    0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                    XCB_COPY_FROM_PARENT, 0, nullptr);
  host_ = xcb_generate_id(connection_);
  const std::uint32_t values[] = {
      screen_->white_pixel,
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
      XCB_EVENT_MASK_BUTTON_PRESS};
  auto created = xcb_create_window_checked(connection_, screen_->root_depth, host_, shell_.panel_window(),
                                           0, tray_y, 1, icon_size, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                           screen_->root_visual,
                                           XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
  if (auto* error = xcb_request_check(connection_, created)) {
    std::free(error);
    release();
    return false;
  }
  constexpr std::uint32_t horizontal = 0;
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, owner_,
                      intern("_NET_SYSTEM_TRAY_ORIENTATION"), XCB_ATOM_CARDINAL, 32, 1, &horizontal);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, owner_,
                      intern("_NET_SYSTEM_TRAY_VISUAL"), XCB_ATOM_VISUALID, 32, 1, &screen_->root_visual);
  xcb_set_selection_owner(connection_, owner_, selection_, XCB_CURRENT_TIME);
  owner_cookie = xcb_get_selection_owner(connection_, selection_);
  Reply<xcb_get_selection_owner_reply_t> acquired(
      xcb_get_selection_owner_reply(connection_, owner_cookie, nullptr), std::free);
  if (!acquired || acquired->owner != owner_) {
    release();
    return false;
  }
  xcb_client_message_event_t manager{};
  manager.response_type = XCB_CLIENT_MESSAGE;
  manager.format = 32;
  manager.window = screen_->root;
  manager.type = intern("MANAGER");
  manager.data.data32[0] = XCB_CURRENT_TIME;
  manager.data.data32[1] = selection_;
  manager.data.data32[2] = owner_;
  xcb_send_event(connection_, 0, screen_->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                 reinterpret_cast<const char*>(&manager));
  xcb_flush(connection_);
  return true;
}

void X11SystemTray::send_xembed(xcb_window_t window, std::uint32_t opcode, std::uint32_t detail) {
  xcb_client_message_event_t message{};
  message.response_type = XCB_CLIENT_MESSAGE;
  message.format = 32;
  message.window = window;
  message.type = xembed_;
  message.data.data32[0] = XCB_CURRENT_TIME;
  message.data.data32[1] = opcode;
  message.data.data32[2] = detail;
  if (opcode == 0) message.data.data32[3] = host_; // XEMBED_EMBEDDED_NOTIFY
  xcb_send_event(connection_, 0, window, XCB_EVENT_MASK_NO_EVENT,
                 reinterpret_cast<const char*>(&message));
}

void X11SystemTray::dock(xcb_window_t window) {
  if (!host_ || !window || window == owner_ || window == host_ ||
      shell_.owns(window) || icons_.contains(window)) return;
  Reply<xcb_get_window_attributes_reply_t> attributes(
      xcb_get_window_attributes_reply(connection_, xcb_get_window_attributes(connection_, window), nullptr),
      std::free);
  if (!attributes || attributes->_class == XCB_WINDOW_CLASS_INPUT_ONLY) return;
  const std::uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  xcb_change_window_attributes(connection_, window, XCB_CW_EVENT_MASK, &events);
  xcb_change_save_set(connection_, XCB_SET_MODE_INSERT, window);
  xcb_reparent_window(connection_, window, host_, 0, 0);
  icons_[window] = {};
  order_.push_back(window);
  refresh(window);
  layout();
  send_xembed(window, 0);
  send_xembed(window, 1); // XEMBED_WINDOW_ACTIVATE
}

void X11SystemTray::refresh(xcb_window_t window) {
  auto it = icons_.find(window);
  if (it == icons_.end()) return;
  auto cookie = xcb_get_property(connection_, 0, window, xembed_info_, xembed_info_, 0, 2);
  Reply<xcb_get_property_reply_t> info(xcb_get_property_reply(connection_, cookie, nullptr), std::free);
  const bool mapped = !info || info->type != xembed_info_ || info->format != 32 ||
                      xcb_get_property_value_length(info.get()) < 8 ||
                      (static_cast<std::uint32_t*>(xcb_get_property_value(info.get()))[1] & 1);
  if (it->second.mapped != mapped) {
    it->second.mapped = mapped;
    layout();
  }
}

void X11SystemTray::remove(xcb_window_t window, bool destroyed) {
  if (!icons_.erase(window)) return;
  std::erase(order_, window);
  if (!destroyed) xcb_change_save_set(connection_, XCB_SET_MODE_DELETE, window);
  layout();
}

void X11SystemTray::layout() {
  if (!host_) return;
  const auto count = std::ranges::count_if(order_, [this](xcb_window_t window) {
    return icons_.at(window).mapped;
  });
  shell_.set_tray_width(count ? tray_padding + int(count) * icon_step : 0);
  const int width = shell_.tray_width();
  const int x = shell_.tray_x();
  const int slots = width > tray_padding ? (width - tray_padding) / icon_step : 0;
  int position = 0;
  for (auto window : order_) {
    const bool show = icons_.at(window).mapped && position < slots;
    if (show) {
      const std::uint32_t values[] = {std::uint32_t(2 + position * icon_step), 0,
                                      icon_size, icon_size, 0};
      xcb_configure_window(connection_, window,
                           XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                           XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
      xcb_map_window(connection_, window);
      ++position;
    } else xcb_unmap_window(connection_, window);
  }
  if (width) {
    const std::uint32_t values[] = {std::uint32_t(x), tray_y, std::uint32_t(width), icon_size};
    xcb_configure_window(connection_, host_, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    xcb_map_window(connection_, host_);
  } else xcb_unmap_window(connection_, host_);
  host_x_ = x;
  host_width_ = width;
  if (width) paint();
}

void X11SystemTray::paint() {
  if (!host_ || !host_width_) return;
  auto depth = xcb_screen_allowed_depths_iterator(screen_);
  xcb_visualtype_t* visual = nullptr;
  for (; depth.rem && !visual; xcb_depth_next(&depth)) {
    auto candidate = xcb_depth_visuals_iterator(depth.data);
    for (; candidate.rem; xcb_visualtype_next(&candidate))
      if (candidate.data->visual_id == screen_->root_visual) { visual = candidate.data; break; }
  }
  if (!visual) return;
  auto* surface = cairo_xcb_surface_create(connection_, host_, visual, host_width_, icon_size);
  auto* cr = cairo_create(surface);
  const auto& color = shell_.panel_color();
  cairo_set_source_rgb(cr, color[0], color[1], color[2]);
  cairo_paint(cr);
  cairo_set_source_rgb(cr, .98, .98, .98);
  cairo_rectangle(cr, 0, 0, host_width_, 1);
  cairo_rectangle(cr, 0, 0, 1, icon_size);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, .28, .29, .31);
  cairo_rectangle(cr, host_width_ - 1, 0, 1, icon_size);
  cairo_rectangle(cr, 0, icon_size - 1, host_width_, 1);
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  cairo_surface_destroy(surface);
}

void X11SystemTray::tick() {
  if (host_ && (host_x_ != shell_.tray_x() || host_width_ != shell_.tray_width())) layout();
}

bool X11SystemTray::handle(xcb_generic_event_t* event) {
  if (!owner_) return false;
  switch (event->response_type & ~0x80) {
  case XCB_CLIENT_MESSAGE: {
    auto* message = reinterpret_cast<xcb_client_message_event_t*>(event);
    if (message->window == owner_ && message->type == opcode_ && message->format == 32) {
      if (message->data.data32[1] == 0) dock(message->data.data32[2]);
      return true;
    }
    break;
  }
  case XCB_SELECTION_CLEAR: {
    auto* clear = reinterpret_cast<xcb_selection_clear_event_t*>(event);
    if (clear->owner == owner_ && clear->selection == selection_) { release(); return true; }
    break;
  }
  case XCB_CONFIGURE_REQUEST: {
    auto* request = reinterpret_cast<xcb_configure_request_event_t*>(event);
    if (icons_.contains(request->window)) { layout(); return true; }
    break;
  }
  case XCB_MAP_REQUEST: {
    auto* request = reinterpret_cast<xcb_map_request_event_t*>(event);
    if (icons_.contains(request->window)) {
      if (icons_.at(request->window).mapped) xcb_map_window(connection_, request->window);
      return true;
    }
    break;
  }
  case XCB_PROPERTY_NOTIFY: {
    auto* changed = reinterpret_cast<xcb_property_notify_event_t*>(event);
    if (icons_.contains(changed->window) && changed->atom == xembed_info_) {
      refresh(changed->window); return true;
    }
    break;
  }
  case XCB_REPARENT_NOTIFY: {
    auto* changed = reinterpret_cast<xcb_reparent_notify_event_t*>(event);
    if (icons_.contains(changed->window) && changed->parent != host_) {
      remove(changed->window, false); return true;
    }
    break;
  }
  case XCB_DESTROY_NOTIFY: {
    auto* destroyed = reinterpret_cast<xcb_destroy_notify_event_t*>(event);
    if (icons_.contains(destroyed->window)) { remove(destroyed->window, true); return true; }
    break;
  }
  case XCB_EXPOSE: {
    auto* exposed = reinterpret_cast<xcb_expose_event_t*>(event);
    if (exposed->window == host_) { if (!exposed->count) paint(); return true; }
    break;
  }
  default: break;
  }
  return false;
}

void X11SystemTray::release() {
  if (!owner_ && !host_) return;
  for (auto window : order_) {
    xcb_unmap_window(connection_, window);
    xcb_reparent_window(connection_, window, screen_->root, 0, 0);
    xcb_change_save_set(connection_, XCB_SET_MODE_DELETE, window);
  }
  icons_.clear();
  order_.clear();
  shell_.set_tray_width(0);
  if (host_) { xcb_destroy_window(connection_, host_); host_ = XCB_WINDOW_NONE; }
  if (owner_) { xcb_destroy_window(connection_, owner_); owner_ = XCB_WINDOW_NONE; }
  xcb_flush(connection_);
}
} // namespace midnight
