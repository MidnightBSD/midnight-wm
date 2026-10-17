#include "dbus_menu_popup.hpp"

#include "dbus_menu.hpp"
#include "shell.hpp"
#include "ui_theme.hpp"

#include <X11/keysym.h>
#include <cairo/cairo-xcb.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ranges>

namespace midnight {
namespace {
constexpr int base_menu_width = 244;
constexpr int base_row_height = 24;
constexpr int base_border = 4;
constexpr int max_depth = 6;

std::string display_label(const std::string& label) {
  std::string result;
  result.reserve(label.size());
  for (std::size_t i = 0; i < label.size(); ++i) {
    if (label[i] == '_') {
      if (i + 1 < label.size() && label[i + 1] == '_') { result += '_'; ++i; }
    } else result += label[i];
  }
  return result;
}

void text(cairo_t* cr, const std::string& value, int x, int y, int width,
          double scale, bool disabled, const ThemeConfig& theme, bool selected) {
  cairo_save(cr);
  if (theme.dark_mode) {
    const auto& color = disabled ? theme.muted_text_color : theme.text_color;
    cairo_set_source_rgb(cr, selected && !disabled ? 1 : color[0],
                         selected && !disabled ? 1 : color[1],
                         selected && !disabled ? 1 : color[2]);
  } else cairo_set_source_rgb(cr, disabled ? .48 : .08, disabled ? .48 : .08, disabled ? .48 : .08);
  auto* layout = pango_cairo_create_layout(cr);
  auto* font = make_ui_font(theme, false, 10, false, scale);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, value.c_str(), -1);
  pango_layout_set_width(layout, width * PANGO_SCALE);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(font);
  g_object_unref(layout);
  cairo_restore(cr);
}
} // namespace

DbusMenuPopup::DbusMenuPopup(xcb_connection_t* connection, xcb_screen_t* screen,
                             Shell& shell, DbusMenu& menu)
    : connection_(connection), screen_(screen), shell_(shell), menu_(menu),
      key_symbols_(xcb_key_symbols_alloc(connection)) {
  menu_.set_changed_handler([this] { changed(); });
}

DbusMenuPopup::~DbusMenuPopup() {
  close();
  menu_.set_changed_handler({});
  if (key_symbols_) xcb_key_symbols_free(key_symbols_);
}

int DbusMenuPopup::scaled(int value) const {
  return std::max(1, static_cast<int>(std::lround(value * scale_)));
}

int DbusMenuPopup::capacity(const Layer& layer) const {
  return std::max(1, (layer.height - 2 * border_) / row_height_);
}

void DbusMenuPopup::sync_geometry() {
  const int width = std::max(1, shell_.screen_width());
  const int height = std::max(1, shell_.screen_height());
  const double scale = std::clamp(shell_.ui_scale(), 1.0, 4.0);
  auto monitors = query_display_monitors(connection_, screen_);
  const auto monitor = choose_display_monitor(monitors, anchor_x_, anchor_y_);
  if (width == screen_width_ && height == screen_height_ && scale == scale_ &&
      monitors == monitors_ && monitor == popup_monitor_) return;
  monitors_ = std::move(monitors);
  popup_monitor_ = monitor;
  screen_width_ = width;
  screen_height_ = height;
  scale_ = scale;
  menu_width_ = std::min(popup_monitor_.width, scaled(base_menu_width));
  row_height_ = scaled(base_row_height);
  border_ = scaled(base_border);
  const int available = std::clamp(popup_monitor_.height - scaled(36), 1, popup_monitor_.height);
  for (auto& layer : layers_) {
    layer.width = menu_width_;
    layer.height = std::min(available, 2 * border_ +
                            static_cast<int>(layer.ids.size()) * row_height_);
    layer.x = std::clamp(layer.x, popup_monitor_.x,
                         std::max(popup_monitor_.x, popup_monitor_.x + popup_monitor_.width - layer.width));
    layer.y = std::clamp(layer.y, popup_monitor_.y,
                         std::max(popup_monitor_.y, popup_monitor_.y + popup_monitor_.height - layer.height));
    layer.scroll = std::clamp(layer.scroll, 0,
                              std::max(0, static_cast<int>(layer.ids.size()) - capacity(layer)));
    const std::uint32_t values[]{static_cast<std::uint32_t>(layer.x),
                                 static_cast<std::uint32_t>(layer.y),
                                 static_cast<std::uint32_t>(layer.width),
                                 static_cast<std::uint32_t>(layer.height)};
    xcb_configure_window(connection_, layer.window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    paint(layer);
  }
  if (!layers_.empty()) xcb_flush(connection_);
}

void DbusMenuPopup::refresh_geometry() {
  sync_geometry();
}

void DbusMenuPopup::open(std::string service, std::string path, int x, int y,
                         std::function<void()> fallback) {
  close();
  anchor_x_ = x;
  anchor_y_ = y;
  sync_geometry();
  if (path.empty() || path == "/NO_DBUSMENU") { if (fallback) fallback(); return; }
  if (!menu_.initialize()) { if (fallback) fallback(); return; }
  fallback_ = std::move(fallback);
  pending_ = true;
  menu_.open(std::move(service), std::move(path));
}

void DbusMenuPopup::close_from(std::size_t depth) {
  while (layers_.size() > depth) {
    xcb_destroy_window(connection_, layers_.back().window);
    layers_.pop_back();
  }
}

void DbusMenuPopup::close() {
  pending_ = false;
  fallback_ = {};
  if (pointer_grabbed_) { xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME); pointer_grabbed_ = false; }
  if (keyboard_grabbed_) { xcb_ungrab_keyboard(connection_, XCB_CURRENT_TIME); keyboard_grabbed_ = false; }
  close_from(0);
  menu_.close();
  xcb_flush(connection_);
}

void DbusMenuPopup::changed() {
  sync_geometry();
  if (!pending_ && layers_.empty()) return;
  if (!menu_.loaded() && !menu_.failed()) return;
  if (menu_.rows().empty()) {
    if (pending_) { auto fallback = std::move(fallback_); close(); if (fallback) fallback(); }
    else close();
    return;
  }
  if (pending_) {
    pending_ = false;
    show_layer(0, anchor_x_, anchor_y_, 0);
    if (layers_.empty()) { auto fallback = std::move(fallback_); close(); if (fallback) fallback(); }
    return;
  }
  for (std::size_t depth = 0; depth < layers_.size(); ++depth) {
    auto& layer = layers_[depth];
    if (depth && std::ranges::find(menu_.rows(), layer.parent_id, &DbusMenuRow::id) ==
                     menu_.rows().end()) { close_from(depth); break; }
    layer.ids.clear();
    for (const auto& row : menu_.rows())
      if (row.parent_id == layer.parent_id && row.id != layer.parent_id && row.visible)
        layer.ids.push_back(row.id);
    if (layer.ids.empty()) {
      if (depth == 0) { close(); return; }
      close_from(depth);
      break;
    }
    const int available = std::clamp(popup_monitor_.height - scaled(36), 1, popup_monitor_.height);
    const int new_height = std::min(available, border_ * 2 +
                                   static_cast<int>(layer.ids.size()) * row_height_);
    if (new_height != layer.height) {
      layer.height = new_height;
      layer.y = std::clamp(layer.y, popup_monitor_.y,
                           std::max(popup_monitor_.y, popup_monitor_.y + popup_monitor_.height - layer.height));
      const std::uint32_t values[] = {std::uint32_t(layer.y), std::uint32_t(layer.height)};
      xcb_configure_window(connection_, layer.window,
                           XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_HEIGHT, values);
    }
    layer.hover = std::min(layer.hover, static_cast<int>(layer.ids.size()) - 1);
    layer.scroll = std::clamp(layer.scroll, 0, std::max(0, static_cast<int>(layer.ids.size()) -
                             capacity(layer)));
    paint(layer);
  }
  for (std::size_t depth = 0; depth < layers_.size() && depth + 1 < max_depth; ++depth) {
    const auto& layer = layers_[depth];
    if (depth + 1 < layers_.size() || layer.hover < 0 ||
        layer.hover >= static_cast<int>(layer.ids.size())) continue;
    const auto it = std::ranges::find(menu_.rows(), layer.ids[layer.hover], &DbusMenuRow::id);
    if (it == menu_.rows().end() || !it->has_children) continue;
    const int right = layer.x + layer.width - 1;
    const int x = right + menu_width_ <= popup_monitor_.x + popup_monitor_.width ?
                    right : layer.x - menu_width_ + 1;
    const int y = layer.y + border_ + (layer.hover - layer.scroll) * row_height_;
    show_layer(it->id, x, y, depth + 1);
  }
}

void DbusMenuPopup::show_layer(int parent_id, int x, int y, std::size_t depth) {
  sync_geometry();
  if (depth >= max_depth) return;
  close_from(depth);
  Layer layer;
  layer.parent_id = parent_id;
  for (const auto& row : menu_.rows())
    if (row.parent_id == parent_id && row.id != parent_id && row.visible) layer.ids.push_back(row.id);
  if (layer.ids.empty()) return;
  const int available = std::clamp(popup_monitor_.height - scaled(36), 1, popup_monitor_.height);
  layer.height = std::min(available, border_ * 2 + static_cast<int>(layer.ids.size()) * row_height_);
  layer.width = menu_width_;
  layer.x = std::clamp(x, popup_monitor_.x,
                       std::max(popup_monitor_.x, popup_monitor_.x + popup_monitor_.width - layer.width));
  layer.y = std::clamp(y, popup_monitor_.y,
                       std::max(popup_monitor_.y, popup_monitor_.y + popup_monitor_.height - layer.height));
  layer.window = xcb_generate_id(connection_);
  const std::uint32_t values[] = {screen_->white_pixel, 1,
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION |
      XCB_EVENT_MASK_KEY_PRESS};
  auto cookie = xcb_create_window_checked(connection_, screen_->root_depth, layer.window, screen_->root,
      layer.x, layer.y, layer.width, layer.height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
      screen_->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
  if (auto* error = xcb_request_check(connection_, cookie)) { std::free(error); return; }
  xcb_map_window(connection_, layer.window);
  layers_.push_back(std::move(layer));
  paint(layers_.back());
  if (depth == 0) {
    auto grabbed = xcb_grab_pointer_reply(connection_, xcb_grab_pointer(connection_, 0,
        layers_.front().window, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), nullptr);
    pointer_grabbed_ = grabbed && grabbed->status == XCB_GRAB_STATUS_SUCCESS;
    std::free(grabbed);
    auto* keyboard_grab = xcb_grab_keyboard_reply(connection_, xcb_grab_keyboard(connection_, 0,
        layers_.front().window, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), nullptr);
    keyboard_grabbed_ = keyboard_grab && keyboard_grab->status == XCB_GRAB_STATUS_SUCCESS;
    std::free(keyboard_grab);
  }
  xcb_flush(connection_);
}

void DbusMenuPopup::paint(const Layer& layer) {
  auto depth = xcb_screen_allowed_depths_iterator(screen_);
  xcb_visualtype_t* visual = nullptr;
  for (; depth.rem && !visual; xcb_depth_next(&depth)) {
    auto candidate = xcb_depth_visuals_iterator(depth.data);
    for (; candidate.rem; xcb_visualtype_next(&candidate))
      if (candidate.data->visual_id == screen_->root_visual) { visual = candidate.data; break; }
  }
  if (!visual) return;
  auto* surface = cairo_xcb_surface_create(connection_, layer.window, visual, layer.width, layer.height);
  auto* cr = cairo_create(surface);
  const auto& theme = shell_.theme();
  const auto color = [&](const std::array<double, 3>& dark, double light) {
    cairo_set_source_rgb(cr, theme.dark_mode ? dark[0] : light,
                        theme.dark_mode ? dark[1] : light, theme.dark_mode ? dark[2] : light);
  };
  color(theme.surface_color, .79);
  cairo_paint(cr);
  color(theme.highlight_color, .97);
  cairo_rectangle(cr, 0, 0, layer.width, 1);
  cairo_rectangle(cr, 0, 0, 1, layer.height);
  cairo_fill(cr);
  color(theme.shadow_color, .31);
  cairo_rectangle(cr, layer.width - 1, 0, 1, layer.height);
  cairo_rectangle(cr, 0, layer.height - 1, layer.width, 1);
  cairo_fill(cr);
  color(theme.button_color, .70);
  cairo_rectangle(cr, border_, border_, scaled(25),
                  std::max(0, layer.height - 2 * border_));
  cairo_fill(cr);
  const int visible_rows = capacity(layer);
  for (int visible = 0; visible < visible_rows; ++visible) {
    const int index = layer.scroll + visible;
    if (index >= static_cast<int>(layer.ids.size())) break;
    const auto id = layer.ids[index];
    const auto it = std::ranges::find(menu_.rows(), id, &DbusMenuRow::id);
    if (it == menu_.rows().end()) continue;
    const int y = border_ + visible * row_height_;
    if (index == layer.hover && !it->separator && it->enabled) {
      const auto& accent = shell_.accent_color();
      cairo_set_source_rgb(cr, accent[0], accent[1], accent[2]);
      cairo_rectangle(cr, border_, y, layer.width - 2 * border_, row_height_);
      cairo_fill(cr);
    }
    if (it->separator) {
      color(theme.highlight_color, .36);
      cairo_rectangle(cr, scaled(34), y + row_height_ / 2,
                      std::max(1, layer.width - scaled(42)), 1);
      cairo_fill(cr);
      continue;
    }
    if (it->toggle_state == 1 && !it->toggle_type.empty())
      text(cr, it->toggle_type == "radio" ? "●" : "✓",
           scaled(9), y + scaled(3), scaled(17), scale_, !it->enabled, theme, index == layer.hover);
    else if (!it->icon_name.empty()) {
      if (auto* icon = shell_.themed_icon_surface(it->icon_name)) {
        cairo_save(cr);
        cairo_translate(cr, scaled(8), y + scaled(3));
        cairo_scale(cr, scale_, scale_);
        cairo_set_source_surface(cr, icon, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
      }
    }
    text(cr, display_label(it->label), scaled(35), y + scaled(3),
         std::max(1, layer.width - scaled(55)), scale_, !it->enabled, theme, index == layer.hover);
    if (it->has_children)
      text(cr, "▶", layer.width - scaled(22), y + scaled(3), scaled(18), scale_, !it->enabled, theme, index == layer.hover);
  }
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  cairo_surface_destroy(surface);
}

int DbusMenuPopup::row_index(const Layer& layer, int y) const {
  if (y < layer.y + border_ || y >= layer.y + layer.height - border_) return -1;
  const int index = layer.scroll + (y - layer.y - border_) / row_height_;
  return index < static_cast<int>(layer.ids.size()) ? index : -1;
}

int DbusMenuPopup::hit(int x, int y, std::size_t& depth) const {
  for (std::size_t i = layers_.size(); i > 0; --i) {
    const auto& layer = layers_[i - 1];
    if (x >= layer.x && x < layer.x + layer.width && y >= layer.y && y < layer.y + layer.height) {
      depth = i - 1;
      return row_index(layer, y);
    }
  }
  return -1;
}

void DbusMenuPopup::hover(std::size_t depth, int index) {
  if (depth >= layers_.size()) return;
  auto& layer = layers_[depth];
  if (index < 0 || index >= static_cast<int>(layer.ids.size())) return;
  if (layer.hover == index && layers_.size() > depth + 1) return;
  layer.hover = index;
  paint(layer);
  const auto it = std::ranges::find(menu_.rows(), layer.ids[index], &DbusMenuRow::id);
  if (it == menu_.rows().end() || !it->enabled || !it->has_children) {
    close_from(depth + 1);
    return;
  }
  menu_.about_to_show(it->id);
  const int right = layer.x + layer.width - 1;
  const int x = right + menu_width_ <= popup_monitor_.x + popup_monitor_.width ?
                  right : layer.x - menu_width_ + 1;
  const int y = layer.y + border_ + (index - layer.scroll) * row_height_;
  show_layer(it->id, x, y, depth + 1);
}

void DbusMenuPopup::activate(std::size_t depth, int index, std::uint32_t time) {
  if (depth >= layers_.size() || index < 0 || index >= static_cast<int>(layers_[depth].ids.size())) return;
  const auto it = std::ranges::find(menu_.rows(), layers_[depth].ids[index], &DbusMenuRow::id);
  if (it == menu_.rows().end() || it->separator || !it->enabled) return;
  if (it->has_children) { hover(depth, index); return; }
  menu_.activate(it->id, time);
  close();
}

bool DbusMenuPopup::handle(xcb_generic_event_t* event) {
  sync_geometry();
  if (!pending_ && layers_.empty()) return false;
  const auto type = event->response_type & ~0x80;
  if (type == XCB_EXPOSE) {
    const auto* expose = reinterpret_cast<xcb_expose_event_t*>(event);
    for (const auto& layer : layers_)
      if (layer.window == expose->window) { if (!expose->count) paint(layer); return true; }
    return false;
  }
  if (type == XCB_BUTTON_PRESS || type == XCB_MOTION_NOTIFY) {
    const auto* button = type == XCB_BUTTON_PRESS ?
        reinterpret_cast<xcb_button_press_event_t*>(event) : nullptr;
    int x{}, y{};
    if (button) { x = button->root_x; y = button->root_y; }
    else {
      const auto* motion = reinterpret_cast<xcb_motion_notify_event_t*>(event);
      x = motion->root_x;
      y = motion->root_y;
    }
    std::size_t depth{};
    const int index = hit(x, y, depth);
    if (index < 0) {
      if (type == XCB_BUTTON_PRESS) { const bool grabbed = pointer_grabbed_; close(); return grabbed; }
      return false;
    }
    if (type == XCB_MOTION_NOTIFY) { hover(depth, index); return true; }
    if (button->detail == 4 || button->detail == 5) {
      auto& layer = layers_[depth];
      const int visible_rows = capacity(layer);
      layer.scroll = std::clamp(layer.scroll + (button->detail == 5 ? 1 : -1), 0,
                                std::max(0, static_cast<int>(layer.ids.size()) - visible_rows));
      paint(layer);
      return true;
    }
    if (button->detail == 1) activate(depth, index, button->time);
    return true;
  }
  if (type == XCB_KEY_PRESS && !layers_.empty()) {
    const auto* press = reinterpret_cast<xcb_key_press_event_t*>(event);
    const auto key = key_symbols_ ? xcb_key_symbols_get_keysym(key_symbols_, press->detail, 0) : 0;
    if (key == XK_Escape) { close(); return true; }
    if (key == XK_Left && layers_.size() > 1) { close_from(layers_.size() - 1); return true; }
    auto depth = layers_.size() - 1;
    auto& layer = layers_[depth];
    if ((key == XK_Up || key == XK_Down) && !layer.ids.empty()) {
      const int step = key == XK_Down ? 1 : -1;
      for (int attempts = 0; attempts < static_cast<int>(layer.ids.size()); ++attempts) {
        const int next = layer.hover < 0 ? (step > 0 ? 0 : static_cast<int>(layer.ids.size()) - 1) :
            (layer.hover + step + static_cast<int>(layer.ids.size())) % layer.ids.size();
        layer.hover = next;
        const auto it = std::ranges::find(menu_.rows(), layer.ids[next], &DbusMenuRow::id);
        if (it != menu_.rows().end() && !it->separator && it->enabled) break;
      }
      const int visible_rows = capacity(layer);
      if (layer.hover < layer.scroll) layer.scroll = layer.hover;
      if (layer.hover >= layer.scroll + visible_rows) layer.scroll = layer.hover - visible_rows + 1;
      paint(layer);
      return true;
    }
    if (key == XK_Right && layer.hover >= 0) { hover(depth, layer.hover); return true; }
    if ((key == XK_Return || key == XK_KP_Enter) && layer.hover >= 0) {
      activate(depth, layer.hover, press->time); return true;
    }
  }
  return false;
}
} // namespace midnight
