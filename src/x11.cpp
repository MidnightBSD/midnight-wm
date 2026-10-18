#include "x11.hpp"
#include "x11_timestamp.hpp"
#include "shell.hpp"
#include "ui_theme.hpp"
#include "window_controls.hpp"
#include "title_bar.hpp"
#include "startup_notification.hpp"
#include "display_scale.hpp"

#include <cairo/cairo-xcb.h>
#include <pango/pangocairo.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/randr.h>
#include <xcb/xinerama.h>
#include <xcb/sync.h>
#include <xcb/xcbext.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace midnight {
namespace {
constexpr int min_width = 80;
constexpr int min_height = 40;

void box(cairo_t* cr, double x, double y, double width, double height,
         double r, double g, double b) {
  cairo_rectangle(cr, x, y, width, height);
  cairo_set_source_rgb(cr, r, g, b);
  cairo_fill(cr);
}

void label(cairo_t* cr, const std::string& text, int x, int y, int width,
           double scale, const ThemeConfig& theme, bool bold = false) {
  auto* layout = pango_cairo_create_layout(cr);
  auto* desc = make_ui_font(theme, bold, 10, false, scale);
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
// EWMH fullscreen indices use Xinerama ordering, which need not match RandR.
std::vector<DisplayMonitor> fullscreen_layout(xcb_connection_t* connection, xcb_screen_t* screen) {
  const auto* extension = xcb_get_extension_data(connection, &xcb_xinerama_id);
  if (extension && extension->present) {
    Reply<xcb_xinerama_is_active_reply_t> active(xcb_xinerama_is_active_reply(connection,
        xcb_xinerama_is_active(connection), nullptr), std::free);
    if (active && active->state) {
      Reply<xcb_xinerama_query_screens_reply_t> reply(xcb_xinerama_query_screens_reply(connection,
          xcb_xinerama_query_screens(connection), nullptr), std::free);
      if (reply) {
        std::vector<DisplayMonitor> result;
        auto it = xcb_xinerama_query_screens_screen_info_iterator(reply.get());
        for (; it.rem; xcb_xinerama_screen_info_next(&it))
          result.push_back({it.data->x_org, it.data->y_org, it.data->width, it.data->height});
        if (!result.empty()) return result;
      }
    }
  }
  // Servers without Xinerama retain useful single-monitor fullscreen support.
  Reply<xcb_get_geometry_reply_t> geometry(xcb_get_geometry_reply(connection,
      xcb_get_geometry(connection, screen->root), nullptr), std::free);
  return {{0, 0, geometry ? geometry->width : screen->width_in_pixels,
                  geometry ? geometry->height : screen->height_in_pixels, true}};
}
} // namespace

X11WindowManager::X11WindowManager(xcb_connection_t* connection, xcb_screen_t* screen, Shell& shell)
  : connection_(connection), screen_(screen), shell_(shell),
    screen_width_(screen->width_in_pixels), screen_height_(screen->height_in_pixels),
    monitors_(query_display_monitors(connection, screen)),
    fullscreen_layout_(fullscreen_layout(connection, screen)) {
  const char* synthetic = std::getenv("MIDNIGHT_WM_TEST_SYNTHETIC_RESIZE");
  accept_synthetic_root_size_ = synthetic && std::string_view(synthetic) == "1";
}

int X11WindowManager::border_pixels() const { return scaled_pixel(shell_.ui_scale(), 5); }
int X11WindowManager::title_height_pixels() const { return scaled_pixel(shell_.ui_scale(), 28); }

X11WindowManager::~X11WindowManager() {
  active_ = XCB_WINDOW_NONE;
  for (auto& [client, frame] : frames_) {
    update_window_state(frame);
    xcb_delete_property(connection_, client, atom("_NET_WM_ALLOWED_ACTIONS"));
    xcb_delete_property(connection_, client, atom("_NET_WM_FULLSCREEN_MONITORS"));
    if (frame.sync_query) xcb_discard_reply(connection_, frame.sync_query);
    if (frame.unresponsive) xcb_delete_property(connection_, client, atom("_NET_WM_VISIBLE_NAME"));
    xcb_reparent_window(connection_, client, screen_->root, frame.x, frame.y);
    xcb_change_save_set(connection_, XCB_SET_MODE_DELETE, client);
    xcb_destroy_window(connection_, frame.frame);
  }
  if (support_window_) {
    xcb_destroy_window(connection_, support_window_);
    xcb_delete_property(connection_, screen_->root, atom("_NET_SUPPORTING_WM_CHECK"));
    xcb_delete_property(connection_, screen_->root, atom("_NET_ACTIVE_WINDOW"));
    xcb_delete_property(connection_, screen_->root, atom("_MIDNIGHT_WM_TIMESTAMP"));
    xcb_delete_property(connection_, screen_->root, atom("_NET_CLIENT_LIST"));
    xcb_delete_property(connection_, screen_->root, atom("_NET_CLIENT_LIST_STACKING"));
  }
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

void X11WindowManager::update_icccm_hints(Frame& frame) {
  frame.size_hints = {};
  xcb_icccm_get_wm_normal_hints_reply(connection_,
      xcb_icccm_get_wm_normal_hints(connection_, frame.client), &frame.size_hints, nullptr);
  xcb_icccm_wm_hints_t hints{};
  frame.accepts_input = true;
  frame.urgent = false;
  if (xcb_icccm_get_wm_hints_reply(connection_, xcb_icccm_get_wm_hints(connection_, frame.client),
                                   &hints, nullptr)) {
    if (hints.flags & XCB_ICCCM_WM_HINT_INPUT) frame.accepts_input = hints.input != 0;
    frame.urgent = (hints.flags & XCB_ICCCM_WM_HINT_X_URGENCY) != 0;
  }
  frame.takes_focus = false;
  frame.supports_ping = false;
  auto cookie = xcb_get_property(connection_, 0, frame.client, atom("WM_PROTOCOLS"), XCB_ATOM_ATOM, 0, 32);
  Reply<xcb_get_property_reply_t> protocols(xcb_get_property_reply(connection_, cookie, nullptr), std::free);
  if (protocols && protocols->type == XCB_ATOM_ATOM && protocols->format == 32) {
    auto* values = static_cast<xcb_atom_t*>(xcb_get_property_value(protocols.get()));
    for (int i = 0; i < xcb_get_property_value_length(protocols.get()) / 4; ++i)
      if (values[i] == atom("WM_TAKE_FOCUS")) frame.takes_focus = true;
      else if (values[i] == atom("_NET_WM_PING")) frame.supports_ping = true;
  }
  frame.transient_for = XCB_WINDOW_NONE;
  xcb_icccm_get_wm_transient_for_reply(connection_,
      xcb_icccm_get_wm_transient_for(connection_, frame.client), &frame.transient_for, nullptr);
}

void X11WindowManager::constrain_size(const Frame& frame, std::uint16_t& width,
                                      std::uint16_t& height) const {
  const auto& hints = frame.size_hints;
  int minimum_w = std::clamp<int>(
      (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) ? hints.min_width :
        scaled_pixel(shell_.ui_scale(), min_width),
      1, std::numeric_limits<std::uint16_t>::max());
  int minimum_h = std::clamp<int>(
      (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) ? hints.min_height :
        scaled_pixel(shell_.ui_scale(), min_height),
      1, std::numeric_limits<std::uint16_t>::max());
  const int maximum_w = (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) && hints.max_width > 0
      ? std::clamp<int>(hints.max_width, 1, std::numeric_limits<std::uint16_t>::max())
      : std::numeric_limits<std::uint16_t>::max();
  const int maximum_h = (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) && hints.max_height > 0
      ? std::clamp<int>(hints.max_height, 1, std::numeric_limits<std::uint16_t>::max())
      : std::numeric_limits<std::uint16_t>::max();
  minimum_w = std::min(minimum_w, maximum_w);
  minimum_h = std::min(minimum_h, maximum_h);
  int w = std::clamp<int>(width, minimum_w, maximum_w);
  int h = std::clamp<int>(height, minimum_h, maximum_h);
  if ((hints.flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) &&
      hints.min_aspect_num > 0 && hints.min_aspect_den > 0 &&
      hints.max_aspect_num > 0 && hints.max_aspect_den > 0) {
    if (std::int64_t(w) * hints.min_aspect_den < std::int64_t(h) * hints.min_aspect_num) {
      const auto wanted = std::int64_t(w) * hints.min_aspect_den / hints.min_aspect_num;
      if (wanted >= minimum_h) h = static_cast<int>(wanted);
      else w = static_cast<int>(std::min<std::int64_t>(maximum_w,
          (std::int64_t(h) * hints.min_aspect_num + hints.min_aspect_den - 1) / hints.min_aspect_den));
    }
    if (std::int64_t(w) * hints.max_aspect_den > std::int64_t(h) * hints.max_aspect_num) {
      const auto wanted = std::int64_t(h) * hints.max_aspect_num / hints.max_aspect_den;
      if (wanted >= minimum_w) w = static_cast<int>(wanted);
      else h = static_cast<int>(std::min<std::int64_t>(maximum_h,
          (std::int64_t(w) * hints.max_aspect_den + hints.max_aspect_num - 1) / hints.max_aspect_num));
    }
  }
  // ICCCM hints are signed client input. Negative bases can overflow the
  // resize increment subtraction even when the actual window is small.
  const int base_w = (hints.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE)
      ? std::clamp<int>(hints.base_width, 0, std::numeric_limits<std::uint16_t>::max()) : 0;
  const int base_h = (hints.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE)
      ? std::clamp<int>(hints.base_height, 0, std::numeric_limits<std::uint16_t>::max()) : 0;
  if (hints.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) {
    if (hints.width_inc > 0 && w >= base_w)
      w = base_w + (w - base_w) / hints.width_inc * hints.width_inc;
    if (hints.height_inc > 0 && h >= base_h)
      h = base_h + (h - base_h) / hints.height_inc * hints.height_inc;
  }
  width = std::clamp(w, minimum_w, maximum_w);
  height = std::clamp(h, minimum_h, maximum_h);
}

void X11WindowManager::fit_dialog(Frame& frame) {
  if (frame.role != WindowRole::dialog || frame.fullscreen ||
      frame.maximized_horz || frame.maximized_vert) return;
  const auto parent = frames_.find(frame.transient_for);
  const auto monitor = parent != frames_.end() ? monitor_for(parent->second) :
      choose_display_monitor(monitors_, frame.x, frame.y);
  const auto area = workarea_for(monitor);
  const int inset = decorated(frame) ? border_pixels() : 0;
  const int title = decorated(frame) ? title_height_pixels() : 0;
  const int maximum_w = std::max(1, area.width - 2 * inset);
  const int maximum_h = std::max(1, area.height - title - inset);
  // Keep feasible application size/aspect/increment hints. A minimum larger
  // than the available monitor must yield so the dialog's buttons stay usable.
  Frame bounded;
  bounded.size_hints = frame.size_hints;
  auto& hints = bounded.size_hints;
  hints.max_width = (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) && hints.max_width > 0
      ? std::min(hints.max_width, maximum_w) : maximum_w;
  hints.max_height = (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) && hints.max_height > 0
      ? std::min(hints.max_height, maximum_h) : maximum_h;
  hints.flags |= XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
  constrain_size(bounded, frame.width, frame.height);
  frame.x = std::clamp(frame.x, area.x,
      std::max(area.x, area.x + area.width - int(frame.width) - 2 * inset));
  frame.y = std::clamp(frame.y, area.y,
      std::max(area.y, area.y + area.height - int(frame.height) - title - inset));
}

void X11WindowManager::update_window_state(const Frame& frame) {
  std::vector<xcb_atom_t> states;
  if (frame.modal) states.push_back(atom("_NET_WM_STATE_MODAL"));
  if (frame.maximized_vert) states.push_back(atom("_NET_WM_STATE_MAXIMIZED_VERT"));
  if (frame.maximized_horz) states.push_back(atom("_NET_WM_STATE_MAXIMIZED_HORZ"));
  if (frame.fullscreen) states.push_back(atom("_NET_WM_STATE_FULLSCREEN"));
  if (frame.shaded) states.push_back(atom("_NET_WM_STATE_SHADED"));
  if (frame.minimized) states.push_back(atom("_NET_WM_STATE_HIDDEN"));
  if (frame.skip_taskbar) states.push_back(atom("_NET_WM_STATE_SKIP_TASKBAR"));
  if (frame.skip_pager) states.push_back(atom("_NET_WM_STATE_SKIP_PAGER"));
  if (frame.above) states.push_back(atom("_NET_WM_STATE_ABOVE"));
  if (frame.below) states.push_back(atom("_NET_WM_STATE_BELOW"));
  if (frame.attention || frame.urgent) states.push_back(atom("_NET_WM_STATE_DEMANDS_ATTENTION"));
  if (frame.client == active_ && frame.visible) states.push_back(atom("_NET_WM_STATE_FOCUSED"));
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, frame.client, atom("_NET_WM_STATE"),
                      XCB_ATOM_ATOM, 32, states.size(), states.data());
  update_allowed_actions(frame);
}

void X11WindowManager::update_allowed_actions(const Frame& frame) {
  std::vector<xcb_atom_t> actions;
  for (const auto* name : {"_NET_WM_ACTION_CLOSE", "_NET_WM_ACTION_CHANGE_DESKTOP",
                          "_NET_WM_ACTION_MINIMIZE", "_NET_WM_ACTION_ABOVE", "_NET_WM_ACTION_BELOW"})
    actions.push_back(atom(name));
  const auto& hints = frame.size_hints;
  const bool has_bounds = (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) &&
                          (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE);
  const bool fixed_w = has_bounds && hints.min_width > 0 && hints.min_width == hints.max_width;
  const bool fixed_h = has_bounds && hints.min_height > 0 && hints.min_height == hints.max_height;
  if (frame.role != WindowRole::splash) {
    actions.push_back(atom("_NET_WM_ACTION_FULLSCREEN"));
    if (!frame.fullscreen) {
      actions.push_back(atom("_NET_WM_ACTION_SHADE"));
      actions.push_back(atom("_NET_WM_ACTION_MAXIMIZE_HORZ"));
      actions.push_back(atom("_NET_WM_ACTION_MAXIMIZE_VERT"));
      if (!frame.maximized_horz && !frame.maximized_vert) {
        actions.push_back(atom("_NET_WM_ACTION_MOVE"));
        if (!frame.shaded && !(fixed_w && fixed_h)) actions.push_back(atom("_NET_WM_ACTION_RESIZE"));
      }
    }
  }
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, frame.client, atom("_NET_WM_ALLOWED_ACTIONS"),
                      XCB_ATOM_ATOM, 32, actions.size(), actions.data());
}

void X11WindowManager::set_attention(Frame& frame, bool attention) {
  frame.attention = attention && frame.client != active_;
  update_window_state(frame);
  update_tasks();
}

void X11WindowManager::clear_attention(Frame& frame) {
  frame.attention = frame.urgent = false;
  xcb_icccm_wm_hints_t hints{};
  if (xcb_icccm_get_wm_hints_reply(connection_, xcb_icccm_get_wm_hints(connection_, frame.client),
                                  &hints, nullptr) && (hints.flags & XCB_ICCCM_WM_HINT_X_URGENCY)) {
    hints.flags &= ~XCB_ICCCM_WM_HINT_X_URGENCY;
    xcb_icccm_set_wm_hints(connection_, frame.client, &hints);
  }
  update_window_state(frame);
}

std::optional<std::uint32_t> X11WindowManager::cardinal(
    xcb_window_t window, xcb_atom_t name, xcb_atom_t type) {
  Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection_,
      xcb_get_property(connection_, false, window, name, type, 0, 1), nullptr), std::free);
  if (!reply || reply->type != type || reply->format != 32 || reply->bytes_after ||
      xcb_get_property_value_length(reply.get()) != 4) return std::nullopt;
  return *static_cast<const std::uint32_t*>(xcb_get_property_value(reply.get()));
}

void X11WindowManager::release_user_time_window(xcb_window_t window) {
  if (!window || std::ranges::any_of(frames_, [window](const auto& entry) {
        return entry.second.user_time_window == window;
      })) return;
  const auto watch = user_time_masks_.find(window);
  if (watch == user_time_masks_.end()) return;
  Reply<xcb_get_window_attributes_reply_t> attrs(xcb_get_window_attributes_reply(connection_,
      xcb_get_window_attributes(connection_, window), nullptr), std::free);
  if (attrs) {
    const auto added = (XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY) & ~watch->second;
    std::uint32_t mask = attrs->your_event_mask & ~added;
    if (frames_.contains(window) || docks_.contains(window))
      mask |= XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(connection_, window, XCB_CW_EVENT_MASK, &mask);
  }
  user_time_masks_.erase(watch);
}

void X11WindowManager::refresh_user_time(Frame& frame) {
  const auto old = frame.user_time_window;
  auto helper = cardinal(frame.client, atom("_NET_WM_USER_TIME_WINDOW"), XCB_ATOM_WINDOW).value_or(0);
  if (helper == frame.client || helper == screen_->root || helper == support_window_ || shell_.owns(helper)) helper = 0;
  if (helper && !user_time_masks_.contains(helper)) {
    Reply<xcb_get_window_attributes_reply_t> attrs(xcb_get_window_attributes_reply(connection_,
        xcb_get_window_attributes(connection_, helper), nullptr), std::free);
    if (attrs) {
      user_time_masks_[helper] = attrs->your_event_mask;
      const std::uint32_t mask = attrs->your_event_mask | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
      xcb_change_window_attributes(connection_, helper, XCB_CW_EVENT_MASK, &mask);
    } else helper = 0;
  }
  frame.user_time_window = helper;
  if (old != helper) release_user_time_window(old);
  frame.user_time = cardinal(helper ? helper : frame.client, atom("_NET_WM_USER_TIME"), XCB_ATOM_CARDINAL);
}

bool X11WindowManager::can_activate(const Frame& frame, std::optional<xcb_timestamp_t> timestamp) const {
  if (frame.role == WindowRole::splash || (!frame.accepts_input && !frame.takes_focus)) return false;
  if (!timestamp) return true; // Preserve legacy clients that provide no interaction hint.
  if (!*timestamp) return false; // Explicit opt-out, even if there is no active window.
  return !last_user_time_ || !timestamp_before(*timestamp, last_user_time_);
}

void X11WindowManager::clear_focus() {
  const auto old = active_;
  active_ = XCB_WINDOW_NONE;
  if (auto previous = frames_.find(old); previous != frames_.end()) {
    update_window_state(previous->second);
    draw_frame(previous->second);
  }
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_ACTIVE_WINDOW"),
                      XCB_ATOM_WINDOW, 32, 1, &active_);
  restack_layers();
  update_stacking();
  update_tasks();
}

void X11WindowManager::observe_event(const xcb_generic_event_t* event) {
  if (!event || (event->response_type & 0x80)) return;
  const auto type = event->response_type & 0x7f;
  if (type == XCB_KEY_PRESS || type == XCB_BUTTON_PRESS) {
    const auto* input = reinterpret_cast<const xcb_key_press_event_t*>(event);
    for (const auto window : {input->event, input->child}) {
      auto client = frame_to_client_.contains(window) ? frame_to_client_.at(window) : window;
      if (auto frame = frames_.find(client); frame != frames_.end()) frame->second.placement_until = {};
    }
    last_event_time_ = input->time;
    if (input->time && (!last_user_time_ || !timestamp_before(input->time, last_user_time_)))
      last_user_time_ = input->time;
  } else if (type == XCB_FOCUS_IN) {
    const auto* focus = reinterpret_cast<const xcb_focus_in_event_t*>(event);
    if (focus->mode == XCB_NOTIFY_MODE_NORMAL && shell_.normal_window(focus->event)) clear_focus();
  }
}

bool X11WindowManager::decorated(const Frame& frame) const {
  return !frame.fullscreen && frame.role != WindowRole::splash;
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
  if (frame.monitor_name && (frame.fullscreen || frame.maximized_horz || frame.maximized_vert))
    for (const auto& monitor : monitors_)
      if (monitor.name == frame.monitor_name) return monitor;
  const int cx = frame.x + frame.width / 2;
  const int cy = frame.y + frame.height / 2;
  return choose_display_monitor(monitors_, cx, cy);
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
  if (const auto* extension = xcb_get_extension_data(connection_, &xcb_randr_id);
      extension && extension->present) {
    randr_event_base_ = extension->first_event;
    xcb_randr_select_input(connection_, screen_->root,
        XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
        XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE | XCB_RANDR_NOTIFY_MASK_RESOURCE_CHANGE);
  }
  const auto* extension = xcb_get_extension_data(connection_, &xcb_sync_id);
  if (extension && extension->present) {
    Reply<xcb_sync_initialize_reply_t> reply(xcb_sync_initialize_reply(connection_,
        xcb_sync_initialize(connection_, 3, 1), nullptr), std::free);
    sync_available_ = bool(reply);
  }
  set_root_properties();
  install_key_grabs();
  scan_existing();
  update_client_list();
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
                          "_NET_CLIENT_LIST_STACKING",
                          "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW", "_NET_WM_ICON", "_NET_WM_NAME", "_NET_WM_VISIBLE_NAME",
                          "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN",
                          "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ",
                          "_NET_WM_STATE_SHADED", "_NET_WM_ACTION_SHADE",
                          "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_MODAL",
                          "_NET_WM_STATE_SKIP_PAGER", "_NET_WM_STATE_ABOVE", "_NET_WM_STATE_BELOW",
                          "_NET_WM_STATE_DEMANDS_ATTENTION", "_NET_WM_STATE_FOCUSED",
                          "_NET_WM_USER_TIME", "_NET_WM_USER_TIME_WINDOW", "_NET_WM_ALLOWED_ACTIONS",
                          "_NET_WM_ACTION_CLOSE", "_NET_WM_ACTION_CHANGE_DESKTOP", "_NET_WM_ACTION_MINIMIZE",
                          "_NET_WM_ACTION_ABOVE", "_NET_WM_ACTION_BELOW", "_NET_WM_ACTION_FULLSCREEN",
                          "_NET_WM_ACTION_MAXIMIZE_HORZ", "_NET_WM_ACTION_MAXIMIZE_VERT",
                          "_NET_WM_ACTION_MOVE", "_NET_WM_ACTION_RESIZE",
                          "_NET_FRAME_EXTENTS", "_NET_REQUEST_FRAME_EXTENTS",
                          "_NET_MOVERESIZE_WINDOW", "_NET_WM_MOVERESIZE",
                          "_NET_RESTACK_WINDOW", "_NET_WM_FULLSCREEN_MONITORS",
                          "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP", "_NET_WORKAREA",
                          "_NET_WM_DESKTOP", "_NET_SHOWING_DESKTOP", "_NET_DESKTOP_NAMES",
                          "_NET_DESKTOP_VIEWPORT", "_NET_DESKTOP_LAYOUT", "_NET_WM_PING",
                          "_NET_DESKTOP_GEOMETRY", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK",
                          "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY",
                          "_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_MENU",
                          "_NET_WM_WINDOW_TYPE_SPLASH",
                          "_NET_WM_STRUT", "_NET_WM_STRUT_PARTIAL"})
    supported.push_back(atom(key));
  if (sync_available_) {
    supported.push_back(atom("_NET_WM_SYNC_REQUEST"));
    supported.push_back(atom("_NET_WM_SYNC_REQUEST_COUNTER"));
  }
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_SUPPORTED"),
                      XCB_ATOM_ATOM, 32, supported.size(), supported.data());
  const std::uint32_t zero = 0;
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_NUMBER_OF_DESKTOPS"),
                      XCB_ATOM_CARDINAL, 32, 1, &desktop_count_);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_CURRENT_DESKTOP"),
                      XCB_ATOM_CARDINAL, 32, 1, &zero);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_SHOWING_DESKTOP"),
                      XCB_ATOM_CARDINAL, 32, 1, &zero);
  constexpr char names[] = "Workspace 1\0Workspace 2\0Workspace 3\0Workspace 4\0";
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_DESKTOP_NAMES"),
                      atom("UTF8_STRING"), 8, sizeof(names) - 1, names);
  const std::array<std::uint32_t, desktop_count_ * 2> viewports{};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_DESKTOP_VIEWPORT"),
                      XCB_ATOM_CARDINAL, 32, viewports.size(), viewports.data());
  update_geometry_properties();
  refresh_desktop_layout();
}

void X11WindowManager::refresh_desktop_layout() {
  desktop_layout_ = DesktopLayout{};
  const auto cookie = xcb_get_property(connection_, 0, screen_->root, atom("_NET_DESKTOP_LAYOUT"),
                                       XCB_ATOM_CARDINAL, 0, 4);
  Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection_, cookie, nullptr), std::free);
  if (!reply || reply->type != XCB_ATOM_CARDINAL || reply->format != 32 || reply->bytes_after) return;
  const auto length = xcb_get_property_value_length(reply.get());
  if (length != 12 && length != 16) return;
  const auto* values = static_cast<const std::uint32_t*>(xcb_get_property_value(reply.get()));
  if (auto layout = DesktopLayout::parse({values, static_cast<std::size_t>(length / 4)}, desktop_count_))
    desktop_layout_ = *layout;
}

void X11WindowManager::refresh_visibility() {
  shell_.set_workspace(current_desktop_, showing_desktop_);
  for (auto& [client, frame] : frames_) {
    const bool visible = frame.mapped && !showing_desktop_ &&
        (frame.desktop == current_desktop_ || frame.desktop == all_desktops_);
    if (visible == frame.visible) continue;
    frame.visible = visible;
    if (visible) xcb_map_window(connection_, frame.frame);
    else xcb_unmap_window(connection_, frame.frame);
    (void)client;
  }
  if (auto it = frames_.find(active_); it != frames_.end() && !it->second.visible) {
    clear_focus();
    xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_POINTER_ROOT, screen_->root, XCB_CURRENT_TIME);
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_ACTIVE_WINDOW"),
                        XCB_ATOM_WINDOW, 32, 1, &active_);
  }
  if (!active_ && !showing_desktop_) cycle_focus();
  restack_layers();
  update_stacking();
  update_tasks();
  xcb_flush(connection_);
}

void X11WindowManager::set_current_desktop(std::uint32_t desktop) {
  if (desktop >= desktop_count_ || desktop == current_desktop_) return;
  end_drag(false);
  current_desktop_ = desktop;
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_CURRENT_DESKTOP"),
                      XCB_ATOM_CARDINAL, 32, 1, &current_desktop_);
  refresh_visibility();
}

void X11WindowManager::set_showing_desktop(bool showing) {
  if (showing == showing_desktop_) return;
  end_drag(false);
  showing_desktop_ = showing;
  const std::uint32_t value = showing;
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_SHOWING_DESKTOP"),
                      XCB_ATOM_CARDINAL, 32, 1, &value);
  refresh_visibility();
}

void X11WindowManager::set_window_desktop(Frame& frame, std::uint32_t desktop) {
  if (desktop >= desktop_count_ && desktop != all_desktops_) return;
  // Move transient descendants together, including malformed cyclic families safely.
  std::set<xcb_window_t> moved{frame.client};
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& [client, candidate] : frames_)
      if (moved.contains(candidate.transient_for) && moved.insert(client).second) changed = true;
  }
  for (const auto client : moved) {
    frames_.at(client).desktop = desktop;
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("_NET_WM_DESKTOP"),
                        XCB_ATOM_CARDINAL, 32, 1, &desktop);
  }
  refresh_visibility();
}

void X11WindowManager::send_protocol(xcb_window_t client, xcb_atom_t protocol,
                                    std::uint32_t value1, std::uint32_t value2, std::uint32_t value3) {
  xcb_client_message_event_t event{};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.format = 32;
  event.window = client;
  event.type = atom("WM_PROTOCOLS");
  event.data.data32[0] = protocol;
  event.data.data32[1] = value1;
  event.data.data32[2] = value2;
  event.data.data32[3] = value3;
  xcb_send_event(connection_, 0, client, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&event));
}

void X11WindowManager::initialize_sync(Frame& frame) {
  if (!sync_available_) return;
  Reply<xcb_get_property_reply_t> protocols(xcb_get_property_reply(connection_,
      xcb_get_property(connection_, 0, frame.client, atom("WM_PROTOCOLS"), XCB_ATOM_ATOM, 0, 64), nullptr), std::free);
  if (!protocols || protocols->format != 32) return;
  const auto* values = static_cast<xcb_atom_t*>(xcb_get_property_value(protocols.get()));
  const auto count = xcb_get_property_value_length(protocols.get()) / 4;
  if (std::find(values, values + count, atom("_NET_WM_SYNC_REQUEST")) == values + count) return;
  Reply<xcb_get_property_reply_t> counter(xcb_get_property_reply(connection_,
      xcb_get_property(connection_, 0, frame.client, atom("_NET_WM_SYNC_REQUEST_COUNTER"),
                       XCB_ATOM_CARDINAL, 0, 1), nullptr), std::free);
  if (!counter || counter->format != 32 || xcb_get_property_value_length(counter.get()) != 4) return;
  const auto id = *static_cast<std::uint32_t*>(xcb_get_property_value(counter.get()));
  if (!id) return;
  // The initial counter value is undefined; EWMH requires resetting it on management.
  if (auto* error = xcb_request_check(connection_, xcb_sync_set_counter_checked(connection_, id, {0, 0}))) {
    std::free(error);
    return;
  }
  frame.sync_counter = id;
}

bool X11WindowManager::resize_pending() const {
  return std::any_of(frames_.begin(), frames_.end(), [](const auto& pair) { return pair.second.sync_waiting; });
}

std::optional<std::pair<std::string, std::string>> X11WindowManager::application_identity(xcb_window_t window) {
  const auto text = [&](xcb_window_t target, const char* property, std::size_t limit) -> std::optional<std::string> {
    Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection_,
      xcb_get_property(connection_, 0, target, atom(property), XCB_ATOM_STRING, 0, (limit + 4) / 4), nullptr), std::free);
    if (!reply) return std::nullopt;
    if (reply->type == XCB_ATOM_NONE) return std::string{};
    const auto size = xcb_get_property_value_length(reply.get());
    if (reply->type != XCB_ATOM_STRING || reply->format != 8 || reply->bytes_after || size < 0 ||
        static_cast<std::size_t>(size) > limit + 1) return std::nullopt;
    std::string value;
    if (size) value.assign(static_cast<const char*>(xcb_get_property_value(reply.get())), size);
    if (!value.empty() && value.back() == '\0') value.pop_back();
    if (value.size() > limit || value.find('\0') != std::string::npos) return std::nullopt;
    return value;
  };
  auto id = text(window, "SM_CLIENT_ID", 256);
  auto role = text(window, "WM_WINDOW_ROLE", 4096);
  if (!id || !role) return std::nullopt;
  if (id->empty()) {
    Reply<xcb_get_property_reply_t> leader(xcb_get_property_reply(connection_,
      xcb_get_property(connection_, 0, window, atom("WM_CLIENT_LEADER"), XCB_ATOM_WINDOW, 0, 2), nullptr), std::free);
    if (!leader || leader->type != XCB_ATOM_WINDOW || leader->format != 32 || leader->bytes_after ||
        xcb_get_property_value_length(leader.get()) != 4) return std::nullopt;
    id = text(*static_cast<xcb_window_t*>(xcb_get_property_value(leader.get())), "SM_CLIENT_ID", 256);
  }
  if (!id || id->empty()) return std::nullopt;
  ApplicationWindow identity;
  identity.client_id = *id; identity.role = *role;
  if (!validate_application_window(identity)) return std::nullopt;
  return std::pair{*id, *role};
}

WindowReport X11WindowManager::application_windows() {
  if (frames_.size() > 512) return std::unexpected("Too many managed windows for checkpoint");
  std::vector<ApplicationWindow> windows;
  std::map<std::pair<std::string, std::string>, std::size_t> counts;
  for (const auto& [client, frame] : frames_) {
    if (frame.role == WindowRole::splash || frame.desktop >= desktop_count_) continue;
    const auto identity = application_identity(client);
    if (!identity) continue;
    ApplicationWindow saved{identity->first, identity->second,
      {frame.fullscreen || frame.maximized_horz ? frame.old_x : frame.x,
       frame.fullscreen || frame.maximized_vert ? frame.old_y : frame.y,
       frame.fullscreen || frame.maximized_horz ? frame.old_width : frame.width,
       frame.fullscreen || frame.maximized_vert ? frame.old_height : frame.height},
      frame.desktop, frame.maximized_horz, frame.maximized_vert, frame.fullscreen, frame.shaded};
    if (!validate_application_window(saved)) continue;
    ++counts[*identity];
    windows.push_back(std::move(saved));
  }
  std::erase_if(windows, [&](const auto& window) { return counts[{window.client_id, window.role}] != 1; });
  return windows;
}

void X11WindowManager::restore_application_windows(std::vector<ApplicationWindow> windows) {
  if (windows.size() > 512 || std::ranges::any_of(windows, [](const auto& w) { return !validate_application_window(w); })) return;
  placements_ = std::move(windows);
  claimed_placements_.clear();
  placement_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(120);
}

void X11WindowManager::apply_application_placements() {
  const auto now = std::chrono::steady_clock::now();
  if (placements_.empty() || frames_.size() > 512 || now < next_placement_check_) return;
  if (now >= placement_deadline_) { placements_.clear(); claimed_placements_.clear(); return; }
  next_placement_check_ = now + std::chrono::milliseconds(100);
  if (std::ranges::none_of(frames_, [&](const auto& item) { return now < item.second.placement_until; })) return;
  std::map<xcb_window_t, std::pair<std::string, std::string>> identities;
  std::map<std::pair<std::string, std::string>, std::size_t> counts;
  for (const auto& [client, frame] : frames_)
    if (auto identity = application_identity(client)) { identities[client] = *identity; ++counts[*identity]; }
  for (auto& [client, frame] : frames_) {
    if (now >= frame.placement_until || frame.role == WindowRole::splash) continue;
    const auto identity = identities.find(client);
    if (identity == identities.end() || counts[identity->second] != 1) continue;
    const auto saved_index = match_application_window(placements_, identity->second.first, identity->second.second, claimed_placements_);
    if (!saved_index) continue;
    const auto saved = placements_[*saved_index];
    claimed_placements_.push_back(*saved_index);
    frame.placement_until = {};
    if (frame.fullscreen) set_fullscreen(client, false);
    if (frame.shaded) set_shaded(client, false);
    if (frame.maximized_horz || frame.maximized_vert) set_maximized(client, false, false);
    if (frame.sync_query) xcb_discard_reply(connection_, frame.sync_query);
    frame.sync_query = 0; frame.sync_waiting = false; frame.sync_deferred = false;
    std::vector<SessionGeometry> areas;
    for (const auto& monitor : monitors_) {
      const auto area = workarea_for(monitor);
      areas.push_back({area.x, area.y, std::max(1, area.width - 2 * border_pixels()),
        std::max(1, area.height - title_height_pixels() - border_pixels())});
    }
    const auto geometry = recover_session_geometry(saved.geometry, areas, 1, 1);
    frame.x = geometry.x; frame.y = geometry.y;
    frame.width = static_cast<std::uint16_t>(geometry.width); frame.height = static_cast<std::uint16_t>(geometry.height);
    constrain_size(frame, frame.width, frame.height);
    resize_frame(frame);
    send_configure_notify(frame);
    set_window_desktop(frame, saved.workspace);
    if (saved.fullscreen) set_fullscreen(client, true);
    else if (saved.maximized_horz || saved.maximized_vert) set_maximized(client, saved.maximized_horz, saved.maximized_vert);
    if (saved.shaded) set_shaded(client, true);
    update_window_state(frame);
  }
}

void X11WindowManager::tick() {
  apply_application_placements();
  const auto now = std::chrono::steady_clock::now();
  bool timestamp_requested = false;
  for (auto& [client, frame] : frames_) {
    if (frame.sync_waiting) {
      bool ready = false;
      if (frame.sync_query) {
        void* raw = nullptr;
        xcb_generic_error_t* error = nullptr;
        if (xcb_poll_for_reply(connection_, frame.sync_query, &raw, &error)) {
          frame.sync_query = 0;
          Reply<xcb_sync_query_counter_reply_t> reply(static_cast<xcb_sync_query_counter_reply_t*>(raw), std::free);
          if (error || !reply) frame.sync_counter = 0;
          else ready = ((std::uint64_t(std::uint32_t(reply->counter_value.hi)) << 32) |
                         reply->counter_value.lo) >= frame.sync_value;
          std::free(error);
        }
      }
      if (now >= frame.sync_deadline) frame.sync_counter = 0; // Broken clients must never freeze resizing.
      if (ready || !frame.sync_counter) {
        frame.sync_waiting = false;
        if (frame.sync_query) xcb_discard_reply(connection_, frame.sync_query);
        frame.sync_query = 0;
        if (frame.sync_deferred) {
          frame.sync_deferred = false;
          resize_frame(frame);
          send_configure_notify(frame);
        }
      } else if (!frame.sync_query) {
        frame.sync_query = xcb_sync_query_counter(connection_, frame.sync_counter).sequence;
      }
    }
    if (!frame.supports_ping || !frame.mapped) continue;
    if (frame.ping_time && now >= frame.ping_deadline && !frame.unresponsive) {
      frame.unresponsive = true;
      const auto visible_name = frame.title + " (Not responding)";
      xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("_NET_WM_VISIBLE_NAME"),
                          atom("UTF8_STRING"), 8, visible_name.size(), visible_name.data());
      draw_frame(frame);
      update_tasks();
    }
    if (now >= frame.next_ping && !frame.ping_time) {
      // An idle server may not have supplied a newer timestamp since the last ping.
      // Ask for PropertyNotify rather than fabricating time or reusing a ping identity.
      if (last_event_time_ == XCB_CURRENT_TIME || last_event_time_ == frame.previous_ping_time) {
        if (!timestamp_requested) {
          xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_MIDNIGHT_WM_TIMESTAMP"),
                              XCB_ATOM_CARDINAL, 32, 1, &client);
          timestamp_requested = true;
        }
        continue;
      }
      frame.ping_time = last_event_time_;
      frame.previous_ping_time = frame.ping_time;
      frame.ping_deadline = now + std::chrono::seconds(5);
      frame.next_ping = now + std::chrono::seconds(10);
      send_protocol(client, atom("_NET_WM_PING"), frame.ping_time, client);
    }
  }
  xcb_flush(connection_);
}

void X11WindowManager::update_geometry_properties() {
  const std::uint32_t geometry[] = {screen_width_, screen_height_};
  const bool panel_at_top = std::any_of(monitors_.begin(), monitors_.end(), [](const auto& monitor) {
    return monitor.primary && monitor.y == 0;
  });
  const auto area = workarea_for({0, 0, screen_width_, screen_height_, panel_at_top});
  const std::uint32_t workarea[] = {std::uint32_t(area.x), std::uint32_t(area.y),
                                   std::uint32_t(area.width), std::uint32_t(area.height)};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_DESKTOP_GEOMETRY"),
                      XCB_ATOM_CARDINAL, 32, 2, geometry);
  std::vector<std::uint32_t> areas;
  for (std::uint32_t i = 0; i < desktop_count_; ++i)
    areas.insert(areas.end(), std::begin(workarea), std::end(workarea));
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_WORKAREA"),
                      XCB_ATOM_CARDINAL, 32, areas.size(), areas.data());
}

X11WindowManager::Monitor X11WindowManager::workarea_for(const Monitor& monitor) const {
  int left = 0, right = 0;
  int top = monitor.primary ? monitor.y + shell_.panel_height() : 0;
  int bottom = 0;
  const auto overlaps = [](int a, int b, std::uint32_t start, std::uint32_t end) {
    return end >= std::uint32_t(std::max(0, a)) && start < std::uint32_t(std::max(0, b));
  };
  for (const auto& [window, dock] : docks_) {
    (void)window;
    if (!dock.mapped) continue;
    const auto& s = dock.strut;
    if (s[0] && overlaps(monitor.y, monitor.y + monitor.height, s[4], s[5]))
      left = std::max(left, int(std::min<std::uint32_t>(s[0], screen_width_)));
    if (s[1] && overlaps(monitor.y, monitor.y + monitor.height, s[6], s[7]))
      right = std::max(right, int(std::min<std::uint32_t>(s[1], screen_width_)));
    if (s[2] && overlaps(monitor.x, monitor.x + monitor.width, s[8], s[9]))
      top = std::max(top, int(std::min<std::uint32_t>(s[2], screen_height_)));
    if (s[3] && overlaps(monitor.x, monitor.x + monitor.width, s[10], s[11]))
      bottom = std::max(bottom, int(std::min<std::uint32_t>(s[3], screen_height_)));
  }
  const int x = std::clamp(left, monitor.x, monitor.x + monitor.width);
  const int y = std::clamp(top, monitor.y, monitor.y + monitor.height);
  const int end_x = std::clamp(int(screen_width_) - right, x, monitor.x + monitor.width);
  const int end_y = std::clamp(int(screen_height_) - bottom, y, monitor.y + monitor.height);
  return {x, y, end_x - x, end_y - y};
}

void X11WindowManager::refresh_dock(xcb_window_t window) {
  auto it = docks_.find(window);
  if (it == docks_.end()) return;
  Dock dock = it->second;
  auto cookie = xcb_get_property(connection_, 0, window, atom("_NET_WM_STRUT_PARTIAL"),
                                 XCB_ATOM_CARDINAL, 0, 12);
  Reply<xcb_get_property_reply_t> partial(xcb_get_property_reply(connection_, cookie, nullptr), std::free);
  if (partial && partial->type == XCB_ATOM_CARDINAL && partial->format == 32 &&
      xcb_get_property_value_length(partial.get()) >= 48) {
    std::memcpy(dock.strut.data(), xcb_get_property_value(partial.get()), 48);
  } else {
    cookie = xcb_get_property(connection_, 0, window, atom("_NET_WM_STRUT"), XCB_ATOM_CARDINAL, 0, 4);
    Reply<xcb_get_property_reply_t> legacy(xcb_get_property_reply(connection_, cookie, nullptr), std::free);
    dock.strut = {};
    if (legacy && legacy->type == XCB_ATOM_CARDINAL && legacy->format == 32 &&
        xcb_get_property_value_length(legacy.get()) >= 16) {
      std::memcpy(dock.strut.data(), xcb_get_property_value(legacy.get()), 16);
      dock.strut[4] = dock.strut[6] = dock.strut[8] = dock.strut[10] = 0;
      dock.strut[5] = dock.strut[7] = screen_height_ ? screen_height_ - 1 : 0;
      dock.strut[9] = dock.strut[11] = screen_width_ ? screen_width_ - 1 : 0;
    }
  }
  if (dock.strut != it->second.strut) {
    it->second.strut = dock.strut;
    update_geometry_properties();
    for (auto& [client, frame] : frames_) {
      (void)client;
      if (frame.maximized_horz || frame.maximized_vert) update_maximized_geometry(frame);
    }
  }
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
  xcb_atom_t role_type = XCB_ATOM_NONE;
  if (types && types->type == XCB_ATOM_ATOM && types->format == 32) {
    auto* values = static_cast<xcb_atom_t*>(xcb_get_property_value(types.get()));
    for (int i = 0; i < xcb_get_property_value_length(types.get()) / 4; ++i) {
      if (values[i] == atom("_NET_WM_WINDOW_TYPE_DOCK")) {
        docks_[window] = Dock{.mapped = attr->map_state != XCB_MAP_STATE_UNMAPPED};
        const std::uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(connection_, window, XCB_CW_EVENT_MASK, &events);
        refresh_dock(window);
        return;
      }
      if (values[i] == atom("_NET_WM_WINDOW_TYPE_DESKTOP")) return;
      if (role_type == XCB_ATOM_NONE &&
          (values[i] == atom("_NET_WM_WINDOW_TYPE_DIALOG") ||
           values[i] == atom("_NET_WM_WINDOW_TYPE_UTILITY") ||
           values[i] == atom("_NET_WM_WINDOW_TYPE_TOOLBAR") ||
           values[i] == atom("_NET_WM_WINDOW_TYPE_MENU") ||
           values[i] == atom("_NET_WM_WINDOW_TYPE_SPLASH"))) role_type = values[i];
    }
  }
  Reply<xcb_get_geometry_reply_t> geometry(
      xcb_get_geometry_reply(connection_, xcb_get_geometry(connection_, window), nullptr), std::free);
  if (!geometry) return;
  Frame frame;
  if (!placements_.empty() && std::chrono::steady_clock::now() < placement_deadline_)
    frame.placement_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  frame.client = window;
  frame.desktop = current_desktop_;
  const auto desktop_cookie = xcb_get_property(connection_, 0, window, atom("_NET_WM_DESKTOP"),
                                               XCB_ATOM_CARDINAL, 0, 1);
  Reply<xcb_get_property_reply_t> desktop_reply(xcb_get_property_reply(connection_, desktop_cookie, nullptr), std::free);
  if (desktop_reply && desktop_reply->format == 32 && xcb_get_property_value_length(desktop_reply.get()) == 4) {
    const auto requested = *static_cast<std::uint32_t*>(xcb_get_property_value(desktop_reply.get()));
    if (requested < desktop_count_ || requested == all_desktops_) frame.desktop = requested;
  }
  frame.frame = xcb_generate_id(connection_);
  frame.x = geometry->x;
  frame.y = std::max<int>(geometry->y, shell_.panel_height());
  frame.width = geometry->width;
  frame.height = geometry->height;
  update_icccm_hints(frame);
  initialize_sync(frame);
  if (role_type == atom("_NET_WM_WINDOW_TYPE_SPLASH")) frame.role = WindowRole::splash;
  else if (role_type == atom("_NET_WM_WINDOW_TYPE_DIALOG") ||
           (role_type == XCB_ATOM_NONE && frame.transient_for)) frame.role = WindowRole::dialog;
  else if (role_type == atom("_NET_WM_WINDOW_TYPE_UTILITY") ||
           role_type == atom("_NET_WM_WINDOW_TYPE_TOOLBAR") ||
           role_type == atom("_NET_WM_WINDOW_TYPE_MENU")) frame.role = WindowRole::utility;
  if (frame.role == WindowRole::splash || frame.transient_for) frame.skip_taskbar = true;
  constrain_size(frame, frame.width, frame.height);
  if (auto parent = frames_.find(frame.transient_for); parent != frames_.end()) {
    frame.desktop = parent->second.desktop;
    frame.x = parent->second.x + (int(parent->second.width) - int(frame.width)) / 2;
    frame.y = parent->second.y + (int(parent->second.height) - int(frame.height)) / 2;
  }
  fit_dialog(frame);
  auto state_cookie = xcb_get_property(connection_, 0, window, atom("_NET_WM_STATE"), XCB_ATOM_ATOM, 0, 32);
  Reply<xcb_get_property_reply_t> initial_states(xcb_get_property_reply(connection_, state_cookie, nullptr), std::free);
  bool initial_horz = false, initial_vert = false, initial_fullscreen = false, initial_shaded = false;
  if (initial_states && initial_states->type == XCB_ATOM_ATOM && initial_states->format == 32) {
    auto* states = static_cast<xcb_atom_t*>(xcb_get_property_value(initial_states.get()));
    for (int i = 0; i < xcb_get_property_value_length(initial_states.get()) / 4; ++i) {
      if (states[i] == atom("_NET_WM_STATE_SKIP_TASKBAR")) frame.skip_taskbar = true;
      if (states[i] == atom("_NET_WM_STATE_SKIP_PAGER")) frame.skip_pager = true;
      if (states[i] == atom("_NET_WM_STATE_ABOVE")) frame.above = true;
      if (states[i] == atom("_NET_WM_STATE_BELOW")) frame.below = true;
      if (states[i] == atom("_NET_WM_STATE_DEMANDS_ATTENTION")) frame.attention = true;
      if (states[i] == atom("_NET_WM_STATE_MODAL")) frame.modal = true;
      if (states[i] == atom("_NET_WM_STATE_FULLSCREEN")) initial_fullscreen = true;
      if (states[i] == atom("_NET_WM_STATE_SHADED")) initial_shaded = true;
      if (states[i] == atom("_NET_WM_STATE_MAXIMIZED_VERT")) initial_vert = true;
      if (states[i] == atom("_NET_WM_STATE_MAXIMIZED_HORZ")) initial_horz = true;
    }
  }
  if (frame.above) frame.below = false;
  frame.ignore_unmap = attr->map_state != XCB_MAP_STATE_UNMAPPED ? 1 : 0;
  frame.mapped = attr->map_state != XCB_MAP_STATE_UNMAPPED;
  const std::uint32_t mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
      XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
      XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  const std::uint32_t values[] = {screen_->white_pixel, mask};
  const int inset = decorated(frame) ? border_pixels() : 0;
  const int top_inset = decorated(frame) ? title_height_pixels() : 0;
  xcb_create_window(connection_, screen_->root_depth, frame.frame, screen_->root,
                    frame.x, frame.y, frame.width + 2 * inset,
                    frame.height + top_inset + inset, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen_->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
  xcb_change_save_set(connection_, XCB_SET_MODE_INSERT, window);
  xcb_reparent_window(connection_, window, frame.frame, inset, top_inset);
  if (frame.role == WindowRole::dialog &&
      (frame.width != geometry->width || frame.height != geometry->height)) {
    const std::uint32_t size[] = {frame.width, frame.height};
    xcb_configure_window(connection_, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
  }
  const std::uint32_t client_events = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  xcb_change_window_attributes(connection_, window, XCB_CW_EVENT_MASK, &client_events);
  const std::uint32_t extents[] = {std::uint32_t(inset), std::uint32_t(inset),
                                   std::uint32_t(top_inset), std::uint32_t(inset)};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window, atom("_NET_FRAME_EXTENTS"),
                      XCB_ATOM_CARDINAL, 32, 4, extents);
  xcb_grab_button(connection_, 0, window, XCB_EVENT_MASK_BUTTON_PRESS,
                  XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE,
                  XCB_CURSOR_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);
  update_title(frame);
  update_icon(frame);
  frame_to_client_[frame.frame] = window;
  frames_[window] = frame;
  xcb_delete_property(connection_, window, atom("_NET_WM_FULLSCREEN_MONITORS"));
  refresh_user_time(frames_.at(window));
  update_window_state(frames_.at(window));
  client_order_.push_back(window);
  if (initial_fullscreen) set_fullscreen(window, true);
  else if (initial_horz || initial_vert) set_maximized(window, initial_horz, initial_vert);
  if (initial_shaded && !initial_fullscreen) set_shaded(window, true);
  if (frame.mapped) map(window);
  update_client_list();
  update_tasks();
}

void X11WindowManager::unmanage(xcb_window_t client, bool destroyed) {
  auto it = frames_.find(client);
  if (it == frames_.end()) return;
  if (drag_client_ == client) end_drag(false);
  auto frame = it->second;
  if (frame.sync_query) xcb_discard_reply(connection_, frame.sync_query);
  if (!destroyed) {
    xcb_reparent_window(connection_, client, screen_->root,
                        frame.x + (decorated(frame) ? border_pixels() : 0),
                        frame.y + (decorated(frame) ? title_height_pixels() : 0));
    xcb_change_save_set(connection_, XCB_SET_MODE_DELETE, client);
    xcb_delete_property(connection_, client, atom("WM_STATE"));
    if (frame.unresponsive) xcb_delete_property(connection_, client, atom("_NET_WM_VISIBLE_NAME"));
    xcb_delete_property(connection_, client, atom("_NET_WM_DESKTOP"));
    xcb_delete_property(connection_, client, atom("_NET_WM_STATE"));
    xcb_delete_property(connection_, client, atom("_NET_WM_ALLOWED_ACTIONS"));
    xcb_delete_property(connection_, client, atom("_NET_WM_FULLSCREEN_MONITORS"));
    xcb_delete_property(connection_, client, atom("_NET_FRAME_EXTENTS"));
  }
  xcb_destroy_window(connection_, frame.frame);
  frame_to_client_.erase(frame.frame);
  frames_.erase(it);
  release_user_time_window(frame.user_time_window);
  std::erase(client_order_, client);
  const bool was_active = active_ == client;
  if (was_active) {
    active_ = XCB_WINDOW_NONE;
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_ACTIVE_WINDOW"),
                        XCB_ATOM_WINDOW, 32, 1, &active_);
  }
  update_client_list();
  update_tasks();
  if (was_active) {
    if (frames_.contains(frame.transient_for) && frames_.at(frame.transient_for).visible) {
      focus(frame.transient_for);
    } else {
      // Query actual stacking: creation order does not track raised windows.
      Reply<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(connection_,
          xcb_query_tree(connection_, screen_->root), nullptr), std::free);
      if (tree) {
        const auto* children = xcb_query_tree_children(tree.get());
        for (int i = xcb_query_tree_children_length(tree.get()); i-- > 0;) {
          const auto found = frame_to_client_.find(children[i]);
          if (found == frame_to_client_.end()) continue;
          const auto& candidate = frames_.at(found->second);
          if (!candidate.visible || candidate.role == WindowRole::splash ||
              (!candidate.accepts_input && !candidate.takes_focus)) continue;
          focus(found->second);
          break;
        }
      }
    }
    if (active_ == XCB_WINDOW_NONE)
      xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_POINTER_ROOT, screen_->root, XCB_CURRENT_TIME);
  }
}

void X11WindowManager::map(xcb_window_t client, bool activate) {
  auto it = frames_.find(client);
  if (it == frames_.end()) return;
  refresh_user_time(it->second);
  const bool permitted = activate && can_activate(it->second, it->second.user_time);
  if (permitted && showing_desktop_ && (it->second.desktop == current_desktop_ || it->second.desktop == all_desktops_))
    set_showing_desktop(false);
  it->second.mapped = true;
  it->second.minimized = false;
  it->second.visible = !showing_desktop_ &&
      (it->second.desktop == current_desktop_ || it->second.desktop == all_desktops_);
  if (it->second.visible) xcb_map_window(connection_, it->second.frame);
  xcb_map_window(connection_, client);
  constexpr std::uint32_t normal[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_WINDOW_NONE};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("WM_STATE"), atom("WM_STATE"), 32, 2, normal);
  update_window_state(it->second);
  if (startup_) startup_->window_mapped(client);
  if (permitted && it->second.visible) focus(client);
  else {
    if (activate && it->second.user_time && *it->second.user_time && !permitted) set_attention(it->second, true);
    if (auto current = frames_.find(active_); current != frames_.end() && current->second.visible &&
        current->first != client && it->second.visible) {
      const std::uint32_t below[] = {current->second.frame, XCB_STACK_MODE_BELOW};
      xcb_configure_window(connection_, it->second.frame,
          XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, below);
    }
    restack_layers(); update_stacking();
  }
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("_NET_WM_DESKTOP"),
                      XCB_ATOM_CARDINAL, 32, 1, &it->second.desktop);
  update_tasks();
}

void X11WindowManager::focus(xcb_window_t client, xcb_timestamp_t time) {
  // A modal transient remains the focused member of its window family.
  std::set<xcb_window_t> checked;
  while (checked.insert(client).second) {
    auto child = std::find_if(client_order_.rbegin(), client_order_.rend(), [&](xcb_window_t candidate) {
      const auto& frame = frames_.at(candidate);
      return frame.visible && frame.modal && frame.transient_for == client;
    });
    if (child == client_order_.rend()) break;
    client = *child;
  }
  auto it = frames_.find(client);
  if (it == frames_.end() || !it->second.visible ||
      (!it->second.accepts_input && !it->second.takes_focus)) return;
  auto old = active_;
  active_ = client;
  clear_attention(it->second);
  if (auto previous = frames_.find(old); previous != frames_.end() && old != client)
    update_window_state(previous->second);
  if (it->second.accepts_input)
    xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_POINTER_ROOT, client, time);
  else if (!it->second.takes_focus)
    xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_INPUT_FOCUS_POINTER_ROOT, time);
  const auto focus_time = time == XCB_CURRENT_TIME ? last_event_time_ : time;
  if (it->second.takes_focus && focus_time != XCB_CURRENT_TIME) {
    xcb_client_message_event_t event{};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = client;
    event.type = atom("WM_PROTOCOLS");
    event.data.data32[0] = atom("WM_TAKE_FOCUS");
    event.data.data32[1] = focus_time;
    xcb_send_event(connection_, 0, client, XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char*>(&event));
  }
  xcb_window_t family = client;
  std::set<xcb_window_t> ancestors;
  while (ancestors.insert(family).second) {
    const auto parent = frames_.at(family).transient_for;
    if (!frames_.contains(parent) || !frames_.at(parent).mapped || ancestors.contains(parent)) break;
    family = parent;
  }
  const std::uint32_t above[] = {XCB_STACK_MODE_ABOVE};
  std::set<xcb_window_t> raised;
  std::function<void(xcb_window_t)> raise_family = [&](xcb_window_t member) {
    if (!raised.insert(member).second || !frames_.at(member).mapped) return;
    xcb_configure_window(connection_, frames_.at(member).frame, XCB_CONFIG_WINDOW_STACK_MODE, above);
    for (const auto child : client_order_)
      if (child != client && frames_.at(child).transient_for == member) raise_family(child);
  };
  if (family != client) raise_family(family);
  raise_family(client);
  restack_layers();
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_ACTIVE_WINDOW"),
                      XCB_ATOM_WINDOW, 32, 1, &client);
  if (auto previous = frames_.find(old); previous != frames_.end()) draw_frame(previous->second);
  draw_frame(it->second);
  update_tasks();
  update_stacking();
}

void X11WindowManager::update_client_list() {
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_CLIENT_LIST"),
                      XCB_ATOM_WINDOW, 32, client_order_.size(), client_order_.data());
  update_stacking();
}

void X11WindowManager::update_stacking() {
  auto cookie = xcb_query_tree(connection_, screen_->root);
  Reply<xcb_query_tree_reply_t> reply(xcb_query_tree_reply(connection_, cookie, nullptr), std::free);
  if (!reply) return;
  std::vector<xcb_window_t> clients;
  auto* children = xcb_query_tree_children(reply.get());
  for (int i = 0; i < xcb_query_tree_children_length(reply.get()); ++i)
    if (auto found = frame_to_client_.find(children[i]); found != frame_to_client_.end())
      clients.push_back(found->second);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, screen_->root, atom("_NET_CLIENT_LIST_STACKING"),
                      XCB_ATOM_WINDOW, 32, clients.size(), clients.data());
}

void X11WindowManager::restack_request(xcb_window_t client, xcb_window_t sibling,
                                       std::uint32_t mode) {
  const auto target = frames_.find(client);
  if (target == frames_.end() || !target->second.visible || mode > XCB_STACK_MODE_OPPOSITE ||
      sibling == client) return;
  std::vector<std::uint32_t> values;
  std::uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;
  if (sibling) {
    const auto other = frames_.find(sibling);
    if (other == frames_.end() || !other->second.visible) return;
    mask |= XCB_CONFIG_WINDOW_SIBLING;
    values.push_back(other->second.frame);
  }
  values.push_back(mode);
  // Both XIDs are root children. Let X11 implement conditional stacking modes,
  // then restore desktop layers and transient ancestry without changing focus.
  xcb_configure_window(connection_, target->second.frame, mask, values.data());
  restack_layers();
  update_stacking();
}

void X11WindowManager::restack_layers(bool force) {
  Reply<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(connection_,
      xcb_query_tree(connection_, screen_->root), nullptr), std::free);
  if (!tree) return;
  const auto* root_children = xcb_query_tree_children(tree.get());
  const std::vector<xcb_window_t> root_order(root_children,
      root_children + xcb_query_tree_children_length(tree.get()));
  if (!force && root_order == last_root_order_) return;
  last_root_order_ = root_order;
  struct Entry { xcb_window_t window; xcb_window_t client; int layer; };
  std::vector<Entry> entries;
  const auto* children = xcb_query_tree_children(tree.get());
  std::set<xcb_window_t> active_family;
  auto ancestor = active_;
  while (frames_.contains(ancestor) && active_family.insert(ancestor).second)
    ancestor = frames_.at(ancestor).transient_for;
  auto layer_for = [this, &active_family](xcb_window_t client) {
    int layer = 2;
    std::set<xcb_window_t> visited;
    while (frames_.contains(client) && visited.insert(client).second) {
      const auto& frame = frames_.at(client);
      const int own = frame.fullscreen && active_family.contains(client) ? 5 : frame.above ? 3 : frame.below ? 1 : 2;
      if (visited.size() == 1) layer = own;
      else layer = std::max(layer, own);
      client = frame.transient_for;
    }
    return layer;
  };
  for (int i = 0; i < xcb_query_tree_children_length(tree.get()); ++i) {
    const auto window = children[i];
    if (const auto found = frame_to_client_.find(window); found != frame_to_client_.end()) {
      if (frames_.at(found->second).visible) entries.push_back({window, found->second, layer_for(found->second)});
    } else {
      Reply<xcb_get_window_attributes_reply_t> attrs(xcb_get_window_attributes_reply(connection_,
          xcb_get_window_attributes(connection_, window), nullptr), std::free);
      if (!attrs || attrs->map_state != XCB_MAP_STATE_VIEWABLE || attrs->_class == XCB_WINDOW_CLASS_INPUT_ONLY) continue;
      int layer = 2;
      if (window == shell_.desktop_window()) layer = 0;
      else if (window == shell_.panel_window() || docks_.contains(window)) layer = 4;
      else if ((shell_.owns(window) && !shell_.normal_window(window)) ||
               (!shell_.owns(window) && attrs->override_redirect)) layer = 6;
      entries.push_back({window, XCB_WINDOW_NONE, layer});
    }
  }
  std::vector<xcb_window_t> current_order;
  for (const auto& entry : entries) current_order.push_back(entry.window);
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return a.layer < b.layer;
  });
  // Stable topological ordering keeps transients above their parents. Break
  // cyclic client hints deterministically, with no recursion on untrusted data.
  std::vector<Entry> ordered;
  std::set<xcb_window_t> emitted;
  while (ordered.size() < entries.size()) {
    bool progress = false;
    for (const auto& entry : entries) {
      if (emitted.contains(entry.window)) continue;
      const auto parent = entry.client ? frames_.at(entry.client).transient_for : xcb_window_t(XCB_WINDOW_NONE);
      const auto found = std::ranges::find_if(entries, [&](const Entry& candidate) {
        return candidate.client && candidate.client == parent && candidate.layer == entry.layer;
      });
      if (found != entries.end() && !emitted.contains(found->window)) continue;
      emitted.insert(entry.window);
      ordered.push_back(entry);
      progress = true;
    }
    if (!progress) {
      const auto found = std::ranges::find_if(entries, [&](const Entry& entry) {
        return !emitted.contains(entry.window);
      });
      emitted.insert(found->window);
      ordered.push_back(*found);
    }
  }
  std::stable_sort(ordered.begin(), ordered.end(), [](const Entry& a, const Entry& b) {
    return a.layer < b.layer;
  });
  bool unchanged = ordered.size() == current_order.size();
  for (std::size_t i = 0; unchanged && i < ordered.size(); ++i)
    unchanged = ordered[i].window == current_order[i];
  if (unchanged) return;
  for (std::size_t i = 1; i < ordered.size(); ++i) {
    const std::uint32_t values[] = {ordered[i - 1].window, XCB_STACK_MODE_ABOVE};
    xcb_configure_window(connection_, ordered[i].window,
                         XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
  }
}

void X11WindowManager::update_tasks() {
  std::vector<Task> tasks;
  for (auto& [client, frame] : frames_)
    if (!frame.skip_taskbar && (frame.desktop == current_desktop_ || frame.desktop == all_desktops_))
      tasks.push_back({client, frame.title + (frame.unresponsive ? " (Not responding)" : ""), client == active_, frame.attention || frame.urgent, frame.icon});
  shell_.set_tasks(std::move(tasks));
}

void X11WindowManager::update_icon(Frame& frame) {
  frame.icon = {};
  const auto cookie = xcb_get_property(connection_, false, frame.client, atom("_NET_WM_ICON"),
                                       XCB_ATOM_CARDINAL, 0, 1024 * 1024);
  Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection_, cookie, nullptr), std::free);
  if (reply && reply->type == XCB_ATOM_CARDINAL && reply->format == 32) {
    const auto count = static_cast<std::size_t>(xcb_get_property_value_length(reply.get())) / 4;
    frame.icon.image = decode_task_icon({static_cast<const std::uint32_t*>(xcb_get_property_value(reply.get())), count});
  }
  const auto class_cookie = xcb_get_property(connection_, false, frame.client, XCB_ATOM_WM_CLASS,
                                             XCB_ATOM_STRING, 0, 256);
  Reply<xcb_get_property_reply_t> cls(xcb_get_property_reply(connection_, class_cookie, nullptr), std::free);
  if (!cls || cls->type != XCB_ATOM_STRING || cls->format != 8 || cls->bytes_after) return;
  std::string_view bytes(static_cast<const char*>(xcb_get_property_value(cls.get())),
                         xcb_get_property_value_length(cls.get()));
  const auto separator = bytes.find('\0');
  if (separator == std::string_view::npos) return;
  const auto tail = bytes.substr(separator + 1);
  const auto end = tail.find('\0');
  if (end == std::string_view::npos) return;
  frame.icon.instance = bytes.substr(0, separator);
  frame.icon.wm_class = tail.substr(0, end);
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
  if (frame.unresponsive) {
    const auto visible_name = frame.title + " (Not responding)";
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, frame.client, atom("_NET_WM_VISIBLE_NAME"),
                        atom("UTF8_STRING"), 8, visible_name.size(), visible_name.data());
  }
}

void X11WindowManager::draw_frame(Frame& frame) {
  if (!decorated(frame)) return;
  auto* visual = root_visual();
  if (!visual) return;
  const int border = border_pixels();
  const int title_height = title_height_pixels();
  const double scale = shell_.ui_scale();
  const int width = (frame.sync_waiting ? frame.sync_width : frame.width) + 2 * border;
  const int height = frame.shaded ? title_height :
      (frame.sync_waiting ? frame.sync_height : frame.height) + title_height + border;
  auto* surface = cairo_xcb_surface_create(connection_, frame.frame, visual, width, height);
  auto* cr = cairo_create(surface);
  const auto& theme = shell_.theme();
  const auto themed_box = [&](double x, double y, double w, double h,
                               const std::array<double, 3>& dark,
                               double r, double g, double b) {
    box(cr, x, y, w, h, theme.dark_mode ? dark[0] : r,
        theme.dark_mode ? dark[1] : g, theme.dark_mode ? dark[2] : b);
  };
  themed_box(0, 0, width, height, theme.surface_color, .78, .79, .80);
  themed_box(0, 0, width, 2, theme.highlight_color, .98, .98, .98);
  themed_box(0, 0, 2, height, theme.highlight_color, .98, .98, .98);
  themed_box(width - 2, 0, 2, height, theme.shadow_color, .25, .27, .29);
  themed_box(0, height - 2, width, 2, theme.shadow_color, .25, .27, .29);
  const bool selected = active_ == frame.client;
  draw_title_bar(cr, border, border, width - 2 * border, title_height - border,
                 theme, selected);
  cairo_set_source_rgb(cr, 1, 1, 1);
  label(cr, frame.title + (frame.unresponsive ? " (Not responding)" : ""), border + scaled_pixel(scale, 9), border + scaled_pixel(scale, 4),
        std::max(1, width - scaled_pixel(scale, 124)), scale, theme, true);
  const int button_y = scaled_pixel(scale, 7);
  for (int i = 0; i < 3; ++i) {
    const int x = width - scaled_pixel(scale, 74) + i * scaled_pixel(scale, 23);
    const int button_width = scaled_pixel(scale, 20);
    const int button_height = scaled_pixel(scale, 17);
    // Warp-style raised controls with font-independent, beveled vector symbols.
    const int edge = scaled_pixel(scale, 1);
    themed_box(x, button_y, button_width, button_height, theme.button_color, .78, .79, .80);
    themed_box(x, button_y, button_width, edge, theme.highlight_color, 1, 1, 1);
    themed_box(x, button_y, edge, button_height, theme.highlight_color, 1, 1, 1);
    themed_box(x + button_width - edge, button_y, edge, button_height, theme.shadow_color, .35, .36, .37);
    themed_box(x, button_y + button_height - edge, button_width, edge, theme.shadow_color, .35, .36, .37);
    // Snap the symbol's bounds to device pixels, including fractional UI scales.
    const int symbol_size = scaled_pixel(scale, 10);
    const auto control = i == 0 ? WindowControl::minimize :
                         i == 1 ? WindowControl::maximize : WindowControl::close;
    draw_window_control(cr, control, x + (button_width - symbol_size) / 2,
                        button_y + (button_height - symbol_size) / 2, symbol_size,
                        theme, frame.maximized_horz && frame.maximized_vert);
  }
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  cairo_surface_destroy(surface);
}

void X11WindowManager::resize_frame(Frame& frame) {
  fit_dialog(frame);
  if (frame.sync_waiting) {
    frame.sync_deferred = true;
    return;
  }
  const bool interactive_resize = drag_client_ == frame.client &&
      (drag_mode_ == DragMode::resize || drag_mode_ == DragMode::keyboard_resize);
  if (interactive_resize && frame.sync_counter) {
    ++frame.sync_value;
    frame.sync_waiting = true;
    frame.sync_width = frame.width;
    frame.sync_height = frame.height;
    frame.sync_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    send_protocol(frame.client, atom("_NET_WM_SYNC_REQUEST"), last_event_time_,
                  std::uint32_t(frame.sync_value), std::uint32_t(frame.sync_value >> 32));
    frame.sync_query = xcb_sync_query_counter(connection_, frame.sync_counter).sequence;
  }
  const bool has_decoration = decorated(frame);
  const std::uint32_t position[] = {static_cast<std::uint32_t>(frame.x), static_cast<std::uint32_t>(frame.y),
                                    static_cast<std::uint32_t>(frame.width + (has_decoration ? 2 * border_pixels() : 0)),
                                    static_cast<std::uint32_t>(frame.shaded ? title_height_pixels() :
                                        frame.height + (has_decoration ? title_height_pixels() + border_pixels() : 0))};
  xcb_configure_window(connection_, frame.frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, position);
  const std::uint32_t dimensions[] = {static_cast<std::uint32_t>(has_decoration ? border_pixels() : 0),
                                      static_cast<std::uint32_t>(has_decoration ? title_height_pixels() : 0),
                                      frame.width, frame.height};
  xcb_configure_window(connection_, frame.client, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, dimensions);
  draw_frame(frame);
}

void X11WindowManager::send_configure_notify(const Frame& frame) {
  if (frame.sync_deferred) return;
  xcb_configure_notify_event_t notify{};
  notify.response_type = XCB_CONFIGURE_NOTIFY;
  notify.event = frame.client;
  notify.window = frame.client;
  notify.x = frame.x + (decorated(frame) ? border_pixels() : 0);
  notify.y = frame.y + (decorated(frame) ? title_height_pixels() : 0);
  notify.width = frame.width;
  notify.height = frame.height;
  xcb_send_event(connection_, 0, frame.client, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                 reinterpret_cast<const char*>(&notify));
}

void X11WindowManager::configure_frame(Frame& frame, std::uint16_t mask, std::int32_t x, std::int32_t y,
                                       std::uint32_t width, std::uint32_t height, std::uint8_t gravity) {
  if (!gravity && (frame.size_hints.flags & XCB_ICCCM_SIZE_HINT_P_WIN_GRAVITY))
    gravity = frame.size_hints.win_gravity;
  if (gravity < XCB_GRAVITY_NORTH_WEST || gravity > XCB_GRAVITY_STATIC)
    gravity = XCB_GRAVITY_NORTH_WEST;
  if (!frame.fullscreen) {
    const int left = decorated(frame) ? border_pixels() : 0;
    const int top = decorated(frame) ? title_height_pixels() : 0;
    const int bottom = decorated(frame) ? border_pixels() : 0;
    const int old_width = frame.width;
    const int old_height = frame.height;
    if (!frame.maximized_horz && (mask & XCB_CONFIG_WINDOW_WIDTH))
      frame.width = std::clamp<std::uint32_t>(width, 1, std::numeric_limits<std::uint16_t>::max());
    if (!frame.maximized_vert && (mask & XCB_CONFIG_WINDOW_HEIGHT))
      frame.height = std::clamp<std::uint32_t>(height, 1, std::numeric_limits<std::uint16_t>::max());
    constrain_size(frame, frame.width, frame.height);
    const bool center_x = gravity == XCB_GRAVITY_NORTH || gravity == XCB_GRAVITY_CENTER ||
                          gravity == XCB_GRAVITY_SOUTH;
    const bool right = gravity == XCB_GRAVITY_NORTH_EAST || gravity == XCB_GRAVITY_EAST ||
                       gravity == XCB_GRAVITY_SOUTH_EAST;
    const bool center_y = gravity == XCB_GRAVITY_WEST || gravity == XCB_GRAVITY_CENTER ||
                          gravity == XCB_GRAVITY_EAST;
    const bool bottom_edge = gravity == XCB_GRAVITY_SOUTH_WEST || gravity == XCB_GRAVITY_SOUTH ||
                             gravity == XCB_GRAVITY_SOUTH_EAST;
    if (!frame.maximized_horz) {
      if (mask & XCB_CONFIG_WINDOW_X)
        frame.x = x - (gravity == XCB_GRAVITY_STATIC ? left : right ? 2 * left : center_x ? left : 0);
      else if (frame.width != old_width)
        frame.x -= right ? int(frame.width) - old_width :
                   center_x ? (int(frame.width) - old_width) / 2 : 0;
    }
    if (!frame.maximized_vert) {
      if (mask & XCB_CONFIG_WINDOW_Y)
        frame.y = y - (gravity == XCB_GRAVITY_STATIC ? top : bottom_edge ? top + bottom :
                       center_y ? (top + bottom) / 2 : 0);
      else if (frame.height != old_height)
        frame.y -= bottom_edge ? int(frame.height) - old_height :
                   center_y ? (int(frame.height) - old_height) / 2 : 0;
    }
    if (frame.maximized_horz || frame.maximized_vert) update_maximized_geometry(frame);
    else resize_frame(frame);
  }
  send_configure_notify(frame);
}

void X11WindowManager::begin_drag(Frame& frame, DragMode mode, int root_x, int root_y,
                                  std::uint8_t direction, std::uint8_t button) {
  if (frame.fullscreen || frame.maximized_horz || frame.maximized_vert) return;
  if (frame.shaded && mode != DragMode::move && mode != DragMode::keyboard_move) return;
  if (drag_mode_ != DragMode::none) end_drag(false);
  auto cookie = xcb_grab_pointer(connection_, 0, frame.frame,
                                XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE,
                                XCB_CURSOR_NONE, XCB_CURRENT_TIME);
  Reply<xcb_grab_pointer_reply_t> pointer(xcb_grab_pointer_reply(connection_, cookie, nullptr), std::free);
  if (!pointer || pointer->status != XCB_GRAB_STATUS_SUCCESS) return;
  auto key_cookie = xcb_grab_keyboard(connection_, 0, frame.frame, XCB_CURRENT_TIME,
                                      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
  Reply<xcb_grab_keyboard_reply_t> keyboard(xcb_grab_keyboard_reply(connection_, key_cookie, nullptr), std::free);
  if (!keyboard || keyboard->status != XCB_GRAB_STATUS_SUCCESS) {
    xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
    return;
  }
  drag_mode_ = mode;
  drag_client_ = frame.client;
  drag_root_x_ = root_x; drag_root_y_ = root_y;
  drag_x_ = frame.x; drag_y_ = frame.y;
  drag_width_ = frame.width; drag_height_ = frame.height;
  drag_direction_ = direction;
  drag_button_ = button;
}

void X11WindowManager::end_drag(bool cancel) {
  if (drag_mode_ == DragMode::none) return;
  if (cancel) {
    if (auto frame = frames_.find(drag_client_); frame != frames_.end()) {
      frame->second.x = drag_x_; frame->second.y = drag_y_;
      frame->second.width = drag_width_; frame->second.height = drag_height_;
      resize_frame(frame->second);
      send_configure_notify(frame->second);
    }
  }
  drag_mode_ = DragMode::none;
  drag_client_ = XCB_WINDOW_NONE;
  xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
  xcb_ungrab_keyboard(connection_, XCB_CURRENT_TIME);
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
  const bool enabled = !(it->second.maximized_horz && it->second.maximized_vert);
  set_maximized(client, enabled, enabled);
}

void X11WindowManager::update_maximized_geometry(Frame& frame) {
  const auto area = workarea_for(monitor_for(frame));
  const bool has_decoration = decorated(frame);
  if (frame.maximized_horz) {
    frame.x = area.x;
    frame.width = std::max(1, area.width - (has_decoration ? 2 * border_pixels() : 0));
  }
  if (frame.maximized_vert) {
    frame.y = area.y;
    frame.height = std::max(1, area.height - (has_decoration ? title_height_pixels() + border_pixels() : 0));
  }
  resize_frame(frame);
}

void X11WindowManager::set_maximized(xcb_window_t client, bool horz, bool vert) {
  auto it = frames_.find(client);
  if (it == frames_.end() || it->second.fullscreen) return;
  auto& frame = it->second;
  if (frame.shaded) set_shaded(client, false);
  if (frame.maximized_horz == horz && frame.maximized_vert == vert) return;
  if (horz && !frame.maximized_horz) {
    frame.old_x = frame.x;
    frame.old_width = frame.width;
  } else if (!horz && frame.maximized_horz) {
    frame.x = frame.old_x;
    frame.width = frame.old_width;
  }
  if (vert && !frame.maximized_vert) {
    frame.old_y = frame.y;
    frame.old_height = frame.height;
  } else if (!vert && frame.maximized_vert) {
    frame.y = frame.old_y;
    frame.height = frame.old_height;
  }
  frame.maximized_horz = horz;
  frame.maximized_vert = vert;
  if (horz || vert) frame.monitor_name = monitor_for(frame).name;
  else frame.monitor_name = XCB_ATOM_NONE;
  update_window_state(frame);
  if (horz || vert) update_maximized_geometry(frame);
  else resize_frame(frame);
}

void X11WindowManager::set_fullscreen_monitors(Frame& frame,
    const std::array<std::uint32_t, 4>& indices) {
  if (frame.role == WindowRole::splash || !fullscreen_monitor_area(fullscreen_layout_, indices)) return;
  frame.fullscreen_monitors = indices;
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, frame.client,
      atom("_NET_WM_FULLSCREEN_MONITORS"), XCB_ATOM_CARDINAL, 32, indices.size(), indices.data());
  if (frame.fullscreen) {
    update_fullscreen_geometry(frame);
    resize_frame(frame);
  }
}

void X11WindowManager::update_fullscreen_geometry(Frame& frame) {
  const auto monitor = monitor_for(frame);
  frame.monitor_name = monitor.name;
  frame.x = monitor.x; frame.y = monitor.y;
  frame.width = monitor.width; frame.height = monitor.height;
  if (frame.fullscreen_monitors)
    if (const auto area = fullscreen_monitor_area(fullscreen_layout_, *frame.fullscreen_monitors)) {
      frame.x = area->x; frame.y = area->y;
      frame.width = area->width; frame.height = area->height;
    }
}

void X11WindowManager::set_shaded(xcb_window_t client, bool enabled) {
  auto it = frames_.find(client);
  if (it == frames_.end() || it->second.shaded == enabled ||
      (enabled && !decorated(it->second))) return;
  if (drag_client_ == client) end_drag(false);
  auto& frame = it->second;
  // Shading clips the still-mapped client; do not iconify it or resize its
  // contents to the title bar. Finish any pending interactive resize first.
  if (frame.sync_query) xcb_discard_reply(connection_, frame.sync_query);
  frame.sync_query = 0;
  frame.sync_waiting = false;
  frame.sync_deferred = false;
  frame.shaded = enabled;
  resize_frame(frame);
  send_configure_notify(frame);
  update_window_state(frame);
}

void X11WindowManager::set_fullscreen(xcb_window_t client, bool enabled) {
  auto it = frames_.find(client);
  if (it == frames_.end() || it->second.role == WindowRole::splash || it->second.fullscreen == enabled) return;
  if (enabled && it->second.shaded) set_shaded(client, false);
  if (enabled && (it->second.maximized_horz || it->second.maximized_vert))
    set_maximized(client, false, false);
  auto& frame = it->second;
  if (enabled) {
    frame.old_x = frame.x; frame.old_y = frame.y;
    frame.old_width = frame.width; frame.old_height = frame.height;
    update_fullscreen_geometry(frame);
  } else {
    frame.monitor_name = XCB_ATOM_NONE;
    frame.x = frame.old_x; frame.y = frame.old_y;
    frame.width = frame.old_width; frame.height = frame.old_height;
  }
  frame.fullscreen = enabled;
  const std::uint32_t extents[] = {static_cast<std::uint32_t>(decorated(frame) ? border_pixels() : 0),
                                   static_cast<std::uint32_t>(decorated(frame) ? border_pixels() : 0),
                                   static_cast<std::uint32_t>(decorated(frame) ? title_height_pixels() : 0),
                                   static_cast<std::uint32_t>(decorated(frame) ? border_pixels() : 0)};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("_NET_FRAME_EXTENTS"),
                      XCB_ATOM_CARDINAL, 32, 4, extents);
  update_window_state(frame);
  resize_frame(frame);
  if (enabled) { restack_layers(); update_stacking(); }
  else update_tasks();
}

void X11WindowManager::minimize(xcb_window_t client) {
  auto it = frames_.find(client);
  if (it == frames_.end()) return;
  xcb_unmap_window(connection_, it->second.frame);
  it->second.mapped = false;
  it->second.visible = false;
  it->second.minimized = true;
  constexpr std::uint32_t iconic[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_WINDOW_NONE};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("WM_STATE"), atom("WM_STATE"), 32, 2, iconic);
  update_window_state(it->second);
  if (active_ == client) {
    clear_focus();
    xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_POINTER_ROOT, screen_->root, XCB_CURRENT_TIME);
  }
  restack_layers();
  update_stacking();
  update_tasks();
}

void X11WindowManager::restore(xcb_window_t client) {
  auto it = frames_.find(client);
  if (it == frames_.end()) return;
  set_shaded(client, false);
  if (it->second.desktop != all_desktops_) set_current_desktop(it->second.desktop);
  set_showing_desktop(false);
  map(client, false);
}

xcb_window_t X11WindowManager::client_for(xcb_window_t window) const {
  if (frames_.contains(window)) return window;
  if (auto found = frame_to_client_.find(window); found != frame_to_client_.end()) return found->second;
  return 0;
}

void X11WindowManager::install_key_grabs() {
  auto* symbols = xcb_key_symbols_alloc(connection_);
  if (!symbols) return;
  xcb_ungrab_key(connection_, XCB_GRAB_ANY, screen_->root, XCB_MOD_MASK_ANY);
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
  grab(XK_Left, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1);
  grab(XK_Right, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1);
  grab(XK_Up, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1);
  grab(XK_Down, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1);
  grab(XK_d, XCB_MOD_MASK_4);
  grab(XK_Tab, XCB_MOD_MASK_1);
  grab(XK_F4, XCB_MOD_MASK_1);
  xcb_key_symbols_free(symbols);
}

void X11WindowManager::cycle_focus(bool expand_shaded) {
  if (frames_.empty()) return;
  auto it = frames_.upper_bound(active_);
  if (it == frames_.end()) it = frames_.begin();
  auto first = it;
  while (!it->second.visible) {
    if (++it == frames_.end()) it = frames_.begin();
    if (it == first) return;
  }
  if (expand_shaded) set_shaded(it->first, false);
  focus(it->first);
}

void X11WindowManager::update_screen_size(std::uint16_t width, std::uint16_t height) {
  refresh_display_layout(width, height);
}

void X11WindowManager::refresh_theme() {
  update_geometry_properties();
  for (auto& [client, frame] : frames_) {
    const std::uint32_t extents[]{static_cast<std::uint32_t>(decorated(frame) ? border_pixels() : 0),
                                  static_cast<std::uint32_t>(decorated(frame) ? border_pixels() : 0),
                                  static_cast<std::uint32_t>(decorated(frame) ? title_height_pixels() : 0),
                                  static_cast<std::uint32_t>(decorated(frame) ? border_pixels() : 0)};
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("_NET_FRAME_EXTENTS"),
                        XCB_ATOM_CARDINAL, 32, 4, extents);
    if (!frame.fullscreen && (frame.maximized_horz || frame.maximized_vert)) update_maximized_geometry(frame);
    else {
      if (!frame.fullscreen) {
        const auto monitor = monitor_for(frame);
        const int top = monitor.y + (monitor.primary ? shell_.panel_height() : 0);
        frame.y = std::clamp(frame.y, top,
                             std::max(top, monitor.y + monitor.height - title_height_pixels()));
      }
      resize_frame(frame);
    }
    draw_frame(frame);
  }
}

void X11WindowManager::refresh_display_layout(std::uint16_t width, std::uint16_t height) {
  auto monitors = query_display_monitors(connection_, screen_);
  auto fullscreen_monitors = fullscreen_layout(connection_, screen_);
  if (monitors.size() == 1 && monitors.front().x == 0 && monitors.front().y == 0 &&
      (monitors.front().width != width || monitors.front().height != height)) {
    monitors.front().width = width;
    monitors.front().height = height;
  }
  if (width == screen_width_ && height == screen_height_ && monitors == monitors_ &&
      fullscreen_monitors == fullscreen_layout_) return;
  screen_width_ = width; screen_height_ = height;
  monitors_ = std::move(monitors);
  fullscreen_layout_ = std::move(fullscreen_monitors);
  if (shell_.screen_width() != width || shell_.screen_height() != height)
    shell_.set_screen_size(width, height);
  else shell_.refresh_display_layout();
  update_geometry_properties();
  for (auto& [client, frame] : frames_) {
    const std::uint32_t extents[]{static_cast<std::uint32_t>(decorated(frame) ? border_pixels() : 0),
                                  static_cast<std::uint32_t>(decorated(frame) ? border_pixels() : 0),
                                  static_cast<std::uint32_t>(decorated(frame) ? title_height_pixels() : 0),
                                  static_cast<std::uint32_t>(decorated(frame) ? border_pixels() : 0)};
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client, atom("_NET_FRAME_EXTENTS"),
                        XCB_ATOM_CARDINAL, 32, 4, extents);
    // Indices may have changed meaning after hotplug; require a fresh request.
    frame.fullscreen_monitors.reset();
    xcb_delete_property(connection_, client, atom("_NET_WM_FULLSCREEN_MONITORS"));
    if (frame.fullscreen) {
      update_fullscreen_geometry(frame);
      resize_frame(frame);
    } else if (frame.maximized_horz || frame.maximized_vert) {
      frame.monitor_name = monitor_for(frame).name;
      update_maximized_geometry(frame);
    }
    else {
      const auto monitor = monitor_for(frame);
      const int visible_width = std::min(80, monitor.width);
      frame.x = std::clamp(frame.x, monitor.x - int(frame.width) + visible_width,
                           monitor.x + monitor.width - visible_width);
      const int top = monitor.primary ? monitor.y + shell_.panel_height() : monitor.y;
      frame.y = std::clamp(frame.y, top,
                           std::max(top, monitor.y + monitor.height - title_height_pixels()));
      resize_frame(frame);
    }
  }
}

void X11WindowManager::handle(xcb_generic_event_t* event) {
  const auto type = event->response_type & ~0x80;
  if (randr_event_base_ &&
      (type == randr_event_base_ + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
       type == randr_event_base_ + XCB_RANDR_NOTIFY)) {
    Reply<xcb_get_geometry_reply_t> geometry(
        xcb_get_geometry_reply(connection_, xcb_get_geometry(connection_, screen_->root), nullptr),
        std::free);
    if (geometry) refresh_display_layout(geometry->width, geometry->height);
    return;
  }
  switch (type) {
  case XCB_MAPPING_NOTIFY: {
    const auto* mapping = reinterpret_cast<xcb_mapping_notify_event_t*>(event);
    if (mapping->request == XCB_MAPPING_KEYBOARD || mapping->request == XCB_MAPPING_MODIFIER)
      install_key_grabs(); // allocates a fresh keysym mapping before regrabbing
    break;
  }
  case XCB_MAP_REQUEST: {
    auto* e = reinterpret_cast<xcb_map_request_event_t*>(event);
    if (shell_.owns(e->window)) break;
    if (!frames_.contains(e->window)) manage(e->window);
    if (frames_.contains(e->window)) map(e->window);
    else {
      xcb_map_window(connection_, e->window);
      if (auto dock = docks_.find(e->window); dock != docks_.end()) {
        dock->second.mapped = true;
        restack_layers();
        update_geometry_properties();
        for (auto& [client, frame] : frames_) {
          (void)client;
          if (frame.maximized_horz || frame.maximized_vert) update_maximized_geometry(frame);
        }
      }
    }
    break;
  }
  case XCB_MAP_NOTIFY: {
    auto* e = reinterpret_cast<xcb_map_notify_event_t*>(event);
    if (shell_.normal_window(e->window)) {
      restack_layers();
      update_stacking();
    }
    if (auto dock = docks_.find(e->window); dock != docks_.end() && !dock->second.mapped) {
      dock->second.mapped = true;
      restack_layers();
      update_geometry_properties();
      for (auto& [client, frame] : frames_) {
        (void)client;
        if (frame.maximized_horz || frame.maximized_vert) update_maximized_geometry(frame);
      }
    }
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
    configure_frame(it->second, e->value_mask, e->x, e->y, e->width, e->height);
    break;
  }
  case XCB_UNMAP_NOTIFY: {
    auto* e = reinterpret_cast<xcb_unmap_notify_event_t*>(event);
    if (auto dock = docks_.find(e->window); dock != docks_.end() && dock->second.mapped) {
      dock->second.mapped = false;
      update_geometry_properties();
      for (auto& [client, frame] : frames_) {
        (void)client;
        if (frame.maximized_horz || frame.maximized_vert) update_maximized_geometry(frame);
      }
    }
    if (auto it = frames_.find(e->window); it != frames_.end()) {
      if (it->second.ignore_unmap > 0) --it->second.ignore_unmap;
      else if (it->second.mapped) unmanage(e->window, false);
    }
    break;
  }
  case XCB_DESTROY_NOTIFY: {
    auto* e = reinterpret_cast<xcb_destroy_notify_event_t*>(event);
    if (user_time_masks_.erase(e->window)) {
      for (auto& [client, frame] : frames_) {
        (void)client;
        if (frame.user_time_window == e->window) {
          frame.user_time_window = XCB_WINDOW_NONE;
          frame.user_time = cardinal(frame.client, atom("_NET_WM_USER_TIME"), XCB_ATOM_CARDINAL);
        }
      }
    }
    if (drag_client_ == e->window) end_drag(false);
    if (docks_.erase(e->window)) {
      update_geometry_properties();
      for (auto& [client, frame] : frames_) {
        (void)client;
        if (frame.maximized_horz || frame.maximized_vert) update_maximized_geometry(frame);
      }
    }
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
    if (e->window == screen_->root && e->atom == atom("_NET_DESKTOP_LAYOUT"))
      refresh_desktop_layout();
    if (!(event->response_type & 0x80)) last_event_time_ = e->time;
    if (e->atom == atom("_NET_WM_USER_TIME") || e->atom == atom("_NET_WM_USER_TIME_WINDOW")) {
      for (auto& [client, frame] : frames_) {
        if (e->window != client && e->window != frame.user_time_window) continue;
        refresh_user_time(frame);
        if (!(event->response_type & 0x80) && client == active_ && frame.user_time && *frame.user_time &&
            !timestamp_before(e->time, *frame.user_time) &&
            (!last_user_time_ || !timestamp_before(*frame.user_time, last_user_time_)))
          last_user_time_ = *frame.user_time;
      }
    }
    if (startup_ && frames_.contains(e->window) &&
        (e->atom == atom("_NET_STARTUP_ID") || e->atom == XCB_ATOM_WM_CLASS))
      startup_->window_mapped(e->window);
    if (docks_.contains(e->window) &&
        (e->atom == atom("_NET_WM_STRUT_PARTIAL") || e->atom == atom("_NET_WM_STRUT")))
      refresh_dock(e->window);
    if (auto it = frames_.find(e->window); it != frames_.end()) {
      if (e->atom == atom("_NET_WM_NAME") || e->atom == XCB_ATOM_WM_NAME) {
        update_title(it->second); draw_frame(it->second); update_tasks();
      } else if (e->atom == atom("_NET_WM_ICON") || e->atom == XCB_ATOM_WM_CLASS) {
        update_icon(it->second);
        update_tasks();
      } else if (e->atom == XCB_ATOM_WM_NORMAL_HINTS || e->atom == XCB_ATOM_WM_HINTS ||
                 e->atom == atom("WM_PROTOCOLS") || e->atom == XCB_ATOM_WM_TRANSIENT_FOR) {
        update_icccm_hints(it->second);
        if (it->first == active_) clear_attention(it->second);
        else update_window_state(it->second);
        update_tasks();
        if (e->atom == XCB_ATOM_WM_TRANSIENT_FOR && it->second.mapped) {
          restack_layers();
          update_stacking();
        }
        if (!it->second.fullscreen) {
          constrain_size(it->second, it->second.width, it->second.height);
          if (it->second.maximized_horz || it->second.maximized_vert)
            update_maximized_geometry(it->second);
          else resize_frame(it->second);
        }
        update_allowed_actions(it->second);
      }
    }
    break;
  }
  case XCB_BUTTON_PRESS: {
    auto* e = reinterpret_cast<xcb_button_press_event_t*>(event);
    if (!(event->response_type & 0x80)) last_event_time_ = e->time;
    const auto client = client_for(e->event);
    if (!client) break;
    if (e->event == client) {
      focus(client, e->time);
      xcb_allow_events(connection_, XCB_ALLOW_REPLAY_POINTER, e->time);
      break;
    }
    auto& f = frames_.at(client);
    focus(client, e->time);
    if (e->detail == 2 && decorated(f) && e->event_y >= border_pixels() &&
        e->event_y < title_height_pixels()) {
      set_shaded(client, !f.shaded);
      break;
    }
    if (e->detail != 1) break;
    if (!decorated(f)) break;
    const int x = e->event_x, y = e->event_y;
    const int button_start = int(f.width + 2 * border_pixels()) - scaled_pixel(shell_.ui_scale(), 74);
    const int border = border_pixels();
    const bool left = x < border;
    const bool right = x >= int(f.width) + border;
    const bool top = y < border;
    const bool bottom = y >= int(f.height) + title_height_pixels();
    const bool edge = left || right || top || bottom;
    if (!edge && y < title_height_pixels() && x > button_start) {
      const int button = (x - button_start) / scaled_pixel(shell_.ui_scale(), 23);
      if (button == 0) minimize(client);
      else if (button == 1) toggle_maximize(client);
      else close(client);
      break;
    }
    if (f.fullscreen || f.maximized_horz || f.maximized_vert) break;
    if (edge) {
      const std::uint8_t direction = top ? (left ? 0 : right ? 2 : 1) :
          bottom ? (left ? 6 : right ? 4 : 5) : left ? 7 : 3;
      begin_drag(f, DragMode::resize, e->root_x, e->root_y, direction, e->detail);
    } else if (y < title_height_pixels()) {
      begin_drag(f, DragMode::move, e->root_x, e->root_y, 8, e->detail);
    }
    break;
  }
  case XCB_MOTION_NOTIFY: {
    auto* e = reinterpret_cast<xcb_motion_notify_event_t*>(event);
    if (drag_mode_ != DragMode::move && drag_mode_ != DragMode::resize) break;
    if (!frames_.contains(drag_client_)) { end_drag(false); break; }
    auto& f = frames_.at(drag_client_);
    if (drag_mode_ == DragMode::move) {
      f.x = drag_x_ + e->root_x - drag_root_x_;
      f.y = drag_y_ + e->root_y - drag_root_y_;
    } else {
      const int dx = e->root_x - drag_root_x_, dy = e->root_y - drag_root_y_;
      const bool left = drag_direction_ == 0 || drag_direction_ == 6 || drag_direction_ == 7;
      const bool right = drag_direction_ == 2 || drag_direction_ == 3 || drag_direction_ == 4;
      const bool top = drag_direction_ == 0 || drag_direction_ == 1 || drag_direction_ == 2;
      const bool bottom = drag_direction_ == 4 || drag_direction_ == 5 || drag_direction_ == 6;
      f.width = std::clamp<int>(int(drag_width_) + (left ? -dx : right ? dx : 0), 1,
                                std::numeric_limits<std::uint16_t>::max());
      f.height = std::clamp<int>(int(drag_height_) + (top ? -dy : bottom ? dy : 0), 1,
                                 std::numeric_limits<std::uint16_t>::max());
      constrain_size(f, f.width, f.height);
      if (left) f.x = drag_x_ + int(drag_width_) - f.width;
      if (top) f.y = drag_y_ + int(drag_height_) - f.height;
    }
    resize_frame(f);
    send_configure_notify(f);
    break;
  }
  case XCB_BUTTON_RELEASE:
    if (drag_mode_ != DragMode::none &&
        (drag_button_ == 0 || reinterpret_cast<xcb_button_release_event_t*>(event)->detail == drag_button_))
      end_drag(false);
    break;
  case XCB_KEY_PRESS: {
    auto* e = reinterpret_cast<xcb_key_press_event_t*>(event);
    if (!(event->response_type & 0x80)) last_event_time_ = e->time;
    auto* symbols = xcb_key_symbols_alloc(connection_);
    if (!symbols) break;
    auto key = xcb_key_symbols_get_keysym(symbols, e->detail, 0);
    xcb_key_symbols_free(symbols);
    if (drag_mode_ != DragMode::none) {
      if (key == XK_Escape) { end_drag(true); break; }
      if (key == XK_Return || key == XK_KP_Enter) { end_drag(false); break; }
      if (drag_mode_ != DragMode::keyboard_move && drag_mode_ != DragMode::keyboard_resize) break;
      auto frame = frames_.find(drag_client_);
      if (frame == frames_.end()) { end_drag(false); break; }
      const int step = (e->state & XCB_MOD_MASK_CONTROL) ? 1 : 10;
      auto& f = frame->second;
      if (drag_mode_ == DragMode::keyboard_move) {
        if (key == XK_Left) f.x -= step;
        else if (key == XK_Right) f.x += step;
        else if (key == XK_Up) f.y -= step;
        else if (key == XK_Down) f.y += step;
        else break;
      } else {
        if (key == XK_Left) f.width = std::max(1, int(f.width) - step);
        else if (key == XK_Right) f.width = std::min<int>(std::numeric_limits<std::uint16_t>::max(), f.width + step);
        else if (key == XK_Up) f.height = std::max(1, int(f.height) - step);
        else if (key == XK_Down) f.height = std::min<int>(std::numeric_limits<std::uint16_t>::max(), f.height + step);
        else break;
        constrain_size(f, f.width, f.height);
      }
      resize_frame(f);
      send_configure_notify(f);
      break;
    }
    if (key == XK_d && (e->state & XCB_MOD_MASK_4)) {
      set_showing_desktop(!showing_desktop_); break;
    }
    if ((e->state & (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1)) ==
        (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1) &&
        (key == XK_Left || key == XK_Right || key == XK_Up || key == XK_Down)) {
      set_current_desktop(desktop_layout_.neighbor(current_desktop_, desktop_count_,
                          key == XK_Left || key == XK_Right, key == XK_Right || key == XK_Down));
      break;
    }
    if (!(e->state & XCB_MOD_MASK_1)) break;
    if (key == XK_Tab) cycle_focus(true);
    else if (key == XK_F4 && active_) close(active_);
    break;
  }
  case XCB_CLIENT_MESSAGE: {
    auto* e = reinterpret_cast<xcb_client_message_event_t*>(event);
    if (e->format != 32) break;
    if (e->type == atom("WM_CHANGE_STATE") && e->data.data32[0] == XCB_ICCCM_WM_STATE_ICONIC)
      minimize(e->window);
    else if (e->type == atom("_NET_CURRENT_DESKTOP")) set_current_desktop(e->data.data32[0]);
    else if (e->type == atom("_NET_SHOWING_DESKTOP") && e->data.data32[0] <= 1)
      set_showing_desktop(e->data.data32[0] != 0);
    else if (e->type == atom("_NET_WM_DESKTOP")) {
      if (auto it = frames_.find(e->window); it != frames_.end())
        set_window_desktop(it->second, e->data.data32[0]);
    } else if (e->type == atom("WM_PROTOCOLS") && e->window == screen_->root &&
               e->data.data32[0] == atom("_NET_WM_PING")) {
      if (auto it = frames_.find(e->data.data32[2]); it != frames_.end() &&
          it->second.ping_time && it->second.ping_time == e->data.data32[1]) {
        it->second.ping_time = 0;
        it->second.unresponsive = false;
        xcb_delete_property(connection_, it->first, atom("_NET_WM_VISIBLE_NAME"));
        draw_frame(it->second);
        update_tasks();
      }
    } else if (e->type == atom("_NET_REQUEST_FRAME_EXTENTS")) {
      const std::uint32_t extents[] = {static_cast<std::uint32_t>(border_pixels()),
                                      static_cast<std::uint32_t>(border_pixels()),
                                      static_cast<std::uint32_t>(title_height_pixels()),
                                      static_cast<std::uint32_t>(border_pixels())};
      xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, e->window, atom("_NET_FRAME_EXTENTS"),
                          XCB_ATOM_CARDINAL, 32, 4, extents);
    } else if (e->type == atom("_NET_ACTIVE_WINDOW")) {
      if (e->format != 32) break;
      if (auto frame = frames_.find(e->window); frame != frames_.end()) {
        const auto source = e->data.data32[0];
        const auto timestamp = e->data.data32[1];
        if (source > 2) break;
        if (source == 2 || active_ == e->window || can_activate(frame->second, timestamp)) {
          restore(e->window);
          focus(e->window);
        } else set_attention(frame->second, true);
      }
    } else if (e->type == atom("_NET_CLOSE_WINDOW")) close(e->window);
    else if (e->type == atom("_NET_RESTACK_WINDOW") && e->format == 32) {
      if (e->data.data32[0] == 2)
        restack_request(e->window, e->data.data32[1], e->data.data32[2]);
    }
    else if (e->type == atom("_NET_WM_FULLSCREEN_MONITORS") && e->format == 32) {
      if (auto frame = frames_.find(e->window); frame != frames_.end() && e->data.data32[4] <= 2)
        set_fullscreen_monitors(frame->second, {e->data.data32[0], e->data.data32[1],
                                               e->data.data32[2], e->data.data32[3]});
    }
    else if (e->type == atom("_NET_MOVERESIZE_WINDOW") && e->format == 32) {
      if (auto frame = frames_.find(e->window); frame != frames_.end()) {
        const auto flags = e->data.data32[0];
        const auto mask = std::uint16_t((flags >> 8) & 0xf);
        const auto gravity = std::uint8_t(flags & 0xff);
        configure_frame(frame->second, mask,
                        std::int32_t(e->data.data32[1]), std::int32_t(e->data.data32[2]),
                        e->data.data32[3], e->data.data32[4], gravity);
      }
    }
    else if (e->type == atom("_NET_WM_MOVERESIZE") && e->format == 32) {
      const auto direction = e->data.data32[2];
      if (direction == 11) {
        if (drag_client_ == e->window) end_drag(true);
      } else if (auto frame = frames_.find(e->window); frame != frames_.end() && frame->second.mapped &&
                 direction <= 10) {
        focus(e->window);
        const auto mode = direction == 8 ? DragMode::move : direction == 10 ? DragMode::keyboard_move :
                          direction == 9 ? DragMode::keyboard_resize : DragMode::resize;
        begin_drag(frame->second, mode, std::int32_t(e->data.data32[0]),
                   std::int32_t(e->data.data32[1]), direction,
                   std::uint8_t(e->data.data32[3]));
      }
    }
    else if (e->type == atom("_NET_WM_STATE")) {
      auto it = frames_.find(e->window);
      if (it == frames_.end() || e->format != 32 || e->data.data32[0] > 2) break;
      const auto action = e->data.data32[0];
      const auto next = [action](bool current) { return action == 2 ? !current : action == 1; };
      const auto first = e->data.data32[1], second = e->data.data32[2];
      if (first == atom("_NET_WM_STATE_FULLSCREEN") || second == atom("_NET_WM_STATE_FULLSCREEN"))
        set_fullscreen(e->window, next(it->second.fullscreen));
      const bool change_vert = first == atom("_NET_WM_STATE_MAXIMIZED_VERT") ||
                               second == atom("_NET_WM_STATE_MAXIMIZED_VERT");
      const bool change_horz = first == atom("_NET_WM_STATE_MAXIMIZED_HORZ") ||
                               second == atom("_NET_WM_STATE_MAXIMIZED_HORZ");
      if (change_vert || change_horz)
        set_maximized(e->window, change_horz ? next(it->second.maximized_horz) : it->second.maximized_horz,
                      change_vert ? next(it->second.maximized_vert) : it->second.maximized_vert);
      if (first == atom("_NET_WM_STATE_SHADED") || second == atom("_NET_WM_STATE_SHADED"))
        set_shaded(e->window, next(it->second.shaded));
      for (const auto state : {first, second == first ? xcb_atom_t(XCB_ATOM_NONE) : second}) {
        if (state == atom("_NET_WM_STATE_ABOVE")) {
          it->second.above = next(it->second.above);
          if (it->second.above) it->second.below = false;
        } else if (state == atom("_NET_WM_STATE_BELOW")) {
          it->second.below = next(it->second.below);
          if (it->second.below) it->second.above = false;
        } else if (state == atom("_NET_WM_STATE_SKIP_PAGER")) {
          it->second.skip_pager = next(it->second.skip_pager);
        } else if (state == atom("_NET_WM_STATE_DEMANDS_ATTENTION")) {
          set_attention(it->second, next(it->second.attention || it->second.urgent));
        }
      }
      update_window_state(it->second);
      restack_layers();
      update_stacking();
      update_tasks();
      if (first == atom("_NET_WM_STATE_SKIP_TASKBAR") || second == atom("_NET_WM_STATE_SKIP_TASKBAR")) {
        it->second.skip_taskbar = next(it->second.skip_taskbar);
        update_window_state(it->second);
        update_tasks();
      }
      if (first == atom("_NET_WM_STATE_MODAL") || second == atom("_NET_WM_STATE_MODAL")) {
        it->second.modal = next(it->second.modal);
        update_window_state(it->second);
        restack_layers();
        update_stacking();
      }
    }
    break;
  }
  case XCB_CONFIGURE_NOTIFY: {
    auto* e = reinterpret_cast<xcb_configure_notify_event_t*>(event);
    // Any client can send a synthetic root ConfigureNotify; only the server
    // reports real root geometry. An empty screen is never valid.
    if (e->window == screen_->root) {
      const bool synthetic = event->response_type & 0x80;
      if ((!synthetic || accept_synthetic_root_size_) && e->width && e->height)
        update_screen_size(e->width, e->height);
    } else if ((frame_to_client_.contains(e->window) || shell_.normal_window(e->window)) &&
               e->event == screen_->root) {
      restack_layers(false);
      update_stacking();
    }
    break;
  }
  default: break;
  }
}
} // namespace midnight
