#include "status_panel.hpp"

#include "shell.hpp"
#include "ui_theme.hpp"
#include "status_notifier.hpp"
#include "dbus_menu_popup.hpp"
#include "tray.hpp"

#include <cairo/cairo-xcb.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace midnight {
namespace {
constexpr int base_icon_size = 18;
constexpr int base_slot_width = 26;
constexpr int base_padding = 4;
constexpr int base_panel_height = 22;

int scaled(int pixels, double scale) {
  return std::max(1, static_cast<int>(std::lround(pixels * scale)));
}

cairo_surface_t* pixmap_surface(const StatusNotifierPixmap& pixmap) {
  if (pixmap.width < 1 || pixmap.height < 1 || pixmap.width > 512 || pixmap.height > 512 ||
      pixmap.argb.size() != std::size_t(pixmap.width) * pixmap.height * 4) return nullptr;
  auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixmap.width, pixmap.height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return nullptr;
  }
  auto* data = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  for (int y = 0; y < pixmap.height; ++y) {
    auto* output = reinterpret_cast<std::uint32_t*>(data + y * stride);
    for (int x = 0; x < pixmap.width; ++x) {
      const auto* source = pixmap.argb.data() + (std::size_t(y) * pixmap.width + x) * 4;
      const std::uint32_t a = source[0];
      output[x] = a << 24 | (source[1] * a / 255) << 16 |
                  (source[2] * a / 255) << 8 | (source[3] * a / 255);
    }
  }
  cairo_surface_mark_dirty(surface);
  return surface;
}
} // namespace

StatusNotifierPanel::StatusNotifierPanel(xcb_connection_t* connection, xcb_screen_t* screen,
                                         Shell& shell, X11SystemTray& tray, StatusNotifier& notifier,
                                         DbusMenuPopup& menu_popup)
    : connection_(connection), screen_(screen), shell_(shell), tray_(tray),
      notifier_(notifier), menu_popup_(menu_popup) {}

StatusNotifierPanel::~StatusNotifierPanel() {
  if (window_) xcb_destroy_window(connection_, window_);
  tray_.set_external_width(0);
  xcb_flush(connection_);
}

bool StatusNotifierPanel::initialize() {
  if (!shell_.panel_window()) return false;
  const int panel_height = scaled(base_panel_height, shell_.ui_scale());
  const int panel_y = std::max(0, (shell_.panel_height() - panel_height) / 2);
  window_ = xcb_generate_id(connection_);
  const std::uint32_t values[] = {screen_->white_pixel,
                                  XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS};
  const auto cookie = xcb_create_window_checked(
      connection_, screen_->root_depth, window_, shell_.panel_window(), 0, panel_y, 1,
      panel_height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
  if (auto* error = xcb_request_check(connection_, cookie)) {
    std::free(error);
    window_ = XCB_WINDOW_NONE;
    return false;
  }
  notifier_.set_changed_handler([this] { refresh_pending_ = true; });
  refresh();
  return true;
}

void StatusNotifierPanel::refresh() {
  if (!window_) return;
  if (!active_menu_key_.empty() &&
      std::ranges::find_if(notifier_.items(), [this](const auto& item) {
        return item.key == active_menu_key_ && item.status != "Passive";
      }) == notifier_.items().end()) {
    menu_popup_.close();
    active_menu_key_.clear();
  }
  visible_keys_.clear();
  for (const auto& item : notifier_.items())
    if (item.status != "Passive") visible_keys_.push_back(item.key);
  tick();
  paint();
}

void StatusNotifierPanel::tick() {
  if (!window_) return;
  if (std::exchange(refresh_pending_, false)) { refresh(); return; }
  const double scale = shell_.ui_scale();
  const int padding = scaled(base_padding, scale);
  const int slot_width = scaled(base_slot_width, scale);
  const int panel_height = scaled(base_panel_height, scale);
  const int panel_y = std::max(0, (shell_.panel_height() - panel_height) / 2);
  tray_.set_external_width(visible_keys_.empty() ? 0 : padding +
                           slot_width * static_cast<int>(visible_keys_.size()));
  const int x = tray_.external_x();
  const int width = tray_.external_width();
  if (x == x_ && width == width_ && panel_height == height_ && slot_width == slot_width_) return;
  x_ = x;
  width_ = width;
  height_ = panel_height;
  slot_width_ = slot_width;
  if (width_) {
    const std::uint32_t values[] = {std::uint32_t(x_), std::uint32_t(panel_y),
                                    std::uint32_t(width_), std::uint32_t(panel_height)};
    xcb_configure_window(connection_, window_, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    xcb_map_window(connection_, window_);
    paint();
  } else xcb_unmap_window(connection_, window_);
}

void StatusNotifierPanel::paint() {
  if (!window_ || !width_) return;
  const double scale = shell_.ui_scale();
  const int icon_size = scaled(base_icon_size, scale);
  const int padding = scaled(base_padding, scale);
  const int slot_width = scaled(base_slot_width, scale);
  const int panel_height = scaled(base_panel_height, scale);
  auto depth = xcb_screen_allowed_depths_iterator(screen_);
  xcb_visualtype_t* visual = nullptr;
  for (; depth.rem && !visual; xcb_depth_next(&depth)) {
    auto candidate = xcb_depth_visuals_iterator(depth.data);
    for (; candidate.rem; xcb_visualtype_next(&candidate))
      if (candidate.data->visual_id == screen_->root_visual) { visual = candidate.data; break; }
  }
  if (!visual) return;
  auto* surface = cairo_xcb_surface_create(connection_, window_, visual, width_, panel_height);
  auto* cr = cairo_create(surface);
  const auto& color = shell_.panel_color();
  cairo_set_source_rgb(cr, color[0], color[1], color[2]);
  cairo_paint(cr);
  const auto& theme = shell_.theme();
  if (theme.dark_mode)
    cairo_set_source_rgb(cr, theme.highlight_color[0], theme.highlight_color[1], theme.highlight_color[2]);
  else cairo_set_source_rgb(cr, .98, .98, .98);
  cairo_rectangle(cr, 0, 0, width_, 1);
  cairo_rectangle(cr, 0, 0, 1, panel_height);
  cairo_fill(cr);
  if (theme.dark_mode)
    cairo_set_source_rgb(cr, theme.shadow_color[0], theme.shadow_color[1], theme.shadow_color[2]);
  else cairo_set_source_rgb(cr, .28, .29, .31);
  cairo_rectangle(cr, width_ - 1, 0, 1, panel_height);
  cairo_rectangle(cr, 0, panel_height - 1, width_, 1);
  cairo_fill(cr);
  const int slots = std::min(static_cast<int>(visible_keys_.size()),
                             std::max(0, (width_ - padding) / slot_width));
  for (int i = 0; i < slots; ++i) {
    const auto it = std::ranges::find_if(notifier_.items(), [this, i](const auto& item) {
      return item.key == visible_keys_[i];
    });
    if (it == notifier_.items().end()) continue;
    const bool attention = it->status == "NeedsAttention";
    const std::string& name = attention && !it->attention_icon_name.empty()
                                ? it->attention_icon_name : it->icon_name;
    auto* icon = shell_.themed_icon_surface(name);
    bool temporary = false;
    if (!icon) { icon = pixmap_surface(it->pixmap); temporary = true; }
    if (!icon) continue;
    const int w = cairo_image_surface_get_width(icon);
    const int h = cairo_image_surface_get_height(icon);
    cairo_save(cr);
    const double icon_scale = std::min(double(icon_size) / w, double(icon_size) / h);
    const int drawn_w = static_cast<int>(std::lround(w * icon_scale));
    const int drawn_h = static_cast<int>(std::lround(h * icon_scale));
    const int slot_x = padding / 2 + i * slot_width;
    const int icon_x = slot_x + (slot_width - drawn_w) / 2;
    const int icon_y = (panel_height - drawn_h) / 2;
    cairo_translate(cr, icon_x, icon_y);
    cairo_scale(cr, icon_scale, icon_scale);
    cairo_set_source_surface(cr, icon, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    if (temporary) cairo_surface_destroy(icon);
  }
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  cairo_surface_destroy(surface);
}

bool StatusNotifierPanel::handle(xcb_generic_event_t* event) {
  if (!window_) return false;
  const auto type = event->response_type & ~0x80;
  if (type == XCB_EXPOSE) {
    const auto* exposed = reinterpret_cast<xcb_expose_event_t*>(event);
    if (exposed->window == window_) { if (!exposed->count) paint(); return true; }
  }
  if (type != XCB_BUTTON_PRESS) return false;
  const auto* press = reinterpret_cast<xcb_button_press_event_t*>(event);
  if (press->event != window_) return false;
  const int padding = scaled(base_padding, shell_.ui_scale());
  const int slot_x = padding / 2;
  const int index = (press->event_x - slot_x) / slot_width_;
  if (press->event_x < slot_x || index < 0 || index >= static_cast<int>(visible_keys_.size()) ||
      slot_x + index * slot_width_ + slot_width_ > width_) return true;
  const auto& key = visible_keys_[index];
  if (press->detail == 4 || press->detail == 5) {
    notifier_.scroll(key, press->detail == 4 ? 120 : -120, "vertical");
  } else if (press->detail == 3) {
    const auto it = std::ranges::find_if(notifier_.items(), [&key](const auto& item) {
      return item.key == key;
    });
    if (it != notifier_.items().end() && !it->menu_path.empty() &&
        it->menu_path != "/NO_DBUSMENU") {
      const auto service = it->service;
      active_menu_key_ = key;
      menu_popup_.open(service, it->menu_path, press->root_x,
                       shell_.panel_y() + shell_.panel_height(),
                       [this, key, x = press->root_x, y = press->root_y] {
                         notifier_.context_menu(key, x, y);
                       });
    } else notifier_.context_menu(key, press->root_x, press->root_y);
  }
  else if (press->detail == 2) notifier_.secondary_activate(key, press->root_x, press->root_y);
  else if (press->detail == 1) {
    const auto it = std::ranges::find_if(notifier_.items(), [&key](const auto& item) {
      return item.key == key;
    });
    if (it != notifier_.items().end() && it->item_is_menu) {
      if (!it->menu_path.empty() && it->menu_path != "/NO_DBUSMENU") {
        const auto service = it->service;
        active_menu_key_ = key;
        menu_popup_.open(service, it->menu_path, press->root_x,
                         shell_.panel_y() + shell_.panel_height(),
                         [this, key, x = press->root_x, y = press->root_y] {
                           notifier_.context_menu(key, x, y);
                         });
      } else notifier_.context_menu(key, press->root_x, press->root_y);
    } else notifier_.activate(key, press->root_x, press->root_y);
  }
  return true;
}
} // namespace midnight
