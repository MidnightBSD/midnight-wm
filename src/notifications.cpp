#include "notifications.hpp"

#include "shell.hpp"
#include "ui_theme.hpp"

#include <cairo/cairo-xcb.h>
#include <gio/gio.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace midnight {
namespace {
constexpr auto service = "org.freedesktop.Notifications";
constexpr auto path = "/org/freedesktop/Notifications";
constexpr int popup_width = 330;
constexpr int popup_height = 90;
constexpr int gap = 8;
constexpr int max_popups = 4;
constexpr int default_timeout = 6000;

constexpr auto interface_xml = R"xml(
<node><interface name="org.freedesktop.Notifications">
  <method name="GetCapabilities"><arg type="as" direction="out"/></method>
  <method name="Notify">
    <arg type="s" direction="in"/><arg type="u" direction="in"/>
    <arg type="s" direction="in"/><arg type="s" direction="in"/>
    <arg type="s" direction="in"/><arg type="as" direction="in"/>
    <arg type="a{sv}" direction="in"/><arg type="i" direction="in"/>
    <arg type="u" direction="out"/>
  </method>
  <method name="CloseNotification"><arg type="u" direction="in"/></method>
  <method name="GetServerInformation">
    <arg type="s" direction="out"/><arg type="s" direction="out"/>
    <arg type="s" direction="out"/><arg type="s" direction="out"/>
  </method>
  <signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
  <signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
</interface></node>)xml";

xcb_visualtype_t* root_visual(xcb_screen_t* screen) {
  auto depth = xcb_screen_allowed_depths_iterator(screen);
  for (; depth.rem; xcb_depth_next(&depth)) {
    auto visual = xcb_depth_visuals_iterator(depth.data);
    for (; visual.rem; xcb_visualtype_next(&visual))
      if (visual.data->visual_id == screen->root_visual) return visual.data;
  }
  return nullptr;
}

void draw_text(cairo_t* cr, const std::string& content, int x, int y, int width,
               int height, bool bold, const ThemeConfig& theme) {
  auto* layout = pango_cairo_create_layout(cr);
  auto* font = make_ui_font(theme, bold, bold ? 10 : 9, false);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, content.c_str(), -1);
  pango_layout_set_width(layout, width * PANGO_SCALE);
  pango_layout_set_height(layout, height * PANGO_SCALE);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(font);
  g_object_unref(layout);
}

// The protocol permits simple markup in the body. Plain-text rendering strips
// it through GLib rather than passing untrusted markup to Pango.
std::string plain_body(const char* body) {
  if (!body || !*body) return {};
  gchar* text = nullptr;
  if (pango_parse_markup(body, -1, 0, nullptr, &text, nullptr, nullptr)) {
    std::string result = text ? text : "";
    g_free(text);
    return result;
  }
  // Invalid markup is still useful as text; Pango escapes it for us here.
  return body;
}
// Decode a bounded thumbnail directly from validated RGB(A) bytes. Never hand
// a client-supplied rowstride or allocation size to an image decoder.
using Image = std::shared_ptr<cairo_surface_t>;
Image hint_image(GVariant* hints, const char* key) {
  auto* value = g_variant_lookup_value(hints, key, G_VARIANT_TYPE("(iiibiiay)"));
  if (!value) return {};
  gint width, height, stride, bits, channels;
  gboolean alpha;
  GVariant* bytes;
  g_variant_get(value, "(iiibii@ay)", &width, &height, &stride, &alpha, &bits, &channels, &bytes);
  gsize length{};
  const auto* data = static_cast<const guint8*>(g_variant_get_fixed_array(bytes, &length, 1));
  Image result;
  if (width > 0 && height > 0 && width <= 4096 && height <= 4096 && bits == 8 &&
      channels == (alpha ? 4 : 3) && stride >= width * channels && stride <= 65536 &&
      length <= 64 * 1024 * 1024 &&
      static_cast<std::uint64_t>(stride) * (height - 1) + width * channels <= length) {
    const double scale = std::min(1.0, 40.0 / std::max(width, height));
    const int w = std::max(1, int(width * scale)), h = std::max(1, int(height * scale));
    result = Image(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h), cairo_surface_destroy);
    auto* target = cairo_image_surface_get_data(result.get());
    if (target) {
      const int target_stride = cairo_image_surface_get_stride(result.get());
      for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(target + y * target_stride);
        for (int x = 0; x < w; ++x) {
          const auto* pixel = data + (y * height / h) * std::size_t(stride) + (x * width / w) * channels;
          const unsigned a = alpha ? pixel[3] : 255;
          row[x] = (a << 24) | ((pixel[0] * a / 255) << 16) |
                   ((pixel[1] * a / 255) << 8) | (pixel[2] * a / 255);
        }
      }
      cairo_surface_mark_dirty(result.get());
    } else result.reset();
  }
  g_variant_unref(bytes);
  g_variant_unref(value);
  return result;
}

std::string icon_path(const char* value) {
  if (!value) return {};
  if (g_str_has_prefix(value, "file://")) {
    gchar* filename = g_filename_from_uri(value, nullptr, nullptr);
    std::string result = filename ? filename : "";
    g_free(filename);
    return result;
  }
  return value;
}
}  // namespace

struct Notifications::Impl {
  struct Popup {
    std::uint32_t id{};
    xcb_window_t window{};
    xcb_pixmap_t pixmap{};
    std::string summary;
    std::string body;
    std::string icon;
    Image image;
    std::vector<std::pair<std::string, std::string>> actions;
    std::string default_action;
    unsigned urgency{1};
    bool resident{};
    int height() const { return popup_height + int((actions.size() + 1) / 2) * 28; }
    std::chrono::steady_clock::time_point deadline{};
    bool expires{};
  };

  xcb_connection_t* connection;
  xcb_screen_t* screen;
  Shell& shell;
  GDBusConnection* bus{};
  GDBusNodeInfo* introspection{};
  guint registration{};
  bool owns_name{};
  std::uint32_t next_id{1};
  std::vector<Popup> popups;

  Impl(xcb_connection_t* c, xcb_screen_t* s, Shell& sh) : connection(c), screen(s), shell(sh) {}
  ~Impl() {
    for (const auto& popup : popups) {
      xcb_destroy_window(connection, popup.window);
      if (popup.pixmap) xcb_free_pixmap(connection, popup.pixmap);
    }
    if (registration) g_dbus_connection_unregister_object(bus, registration);
    if (owns_name && bus)
      g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", service),
                                  G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
    if (introspection) g_dbus_node_info_unref(introspection);
    if (bus) g_object_unref(bus);
    xcb_flush(connection);
  }

  void emit_closed(std::uint32_t id, std::uint32_t reason) {
    if (bus && owns_name)
      g_dbus_connection_emit_signal(bus, nullptr, path, service, "NotificationClosed",
                                    g_variant_new("(uu)", id, reason), nullptr);
  }

  bool close(std::uint32_t id, std::uint32_t reason) {
    auto it = std::ranges::find(popups, id, &Popup::id);
    if (it == popups.end()) return false;
    xcb_destroy_window(connection, it->window);
    if (it->pixmap) xcb_free_pixmap(connection, it->pixmap);
    popups.erase(it);
    reposition();
    emit_closed(id, reason);
    return true;
  }

  void reposition() {
    const int width = std::min(popup_width, std::max(1, int(screen->width_in_pixels) - 2 * gap));
    const int x = std::max(0, int(screen->width_in_pixels) - width - gap);
    int top = gap + 34;
    for (std::size_t i = 0; i < popups.size(); ++i) {
      const int y = std::max(34, std::min(top, int(screen->height_in_pixels) - popups[i].height() - gap));
      top += popups[i].height() + gap;
      const std::uint32_t coordinates[] = {std::uint32_t(x), std::uint32_t(y)};
      xcb_configure_window(connection, popups[i].window,
                           XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coordinates);
    }
  }

  void paint(Popup& popup) {
    auto* visual = root_visual(screen);
    if (!visual) return;
    const int width = std::min(popup_width, std::max(1, int(screen->width_in_pixels) - 2 * gap));
    const auto pixmap = xcb_generate_id(connection);
    auto cookie = xcb_create_pixmap_checked(connection, screen->root_depth, pixmap,
                                            screen->root, width, popup.height());
    if (auto* error = xcb_request_check(connection, cookie)) { std::free(error); return; }
    auto* surface = cairo_xcb_surface_create(connection, pixmap, visual, width, popup.height());
    auto* cr = cairo_create(surface);
    const auto& theme = shell.theme();
    const auto color = [&](const std::array<double, 3>& dark, double r, double g, double b) {
      cairo_set_source_rgb(cr, theme.dark_mode ? dark[0] : r,
                          theme.dark_mode ? dark[1] : g, theme.dark_mode ? dark[2] : b);
    };
    color(theme.surface_color, .88, .85, .93);
    cairo_paint(cr);
    color(theme.highlight_color, .99, .99, .99);
    cairo_rectangle(cr, 0, 0, width, 2);
    cairo_rectangle(cr, 0, 0, 2, popup.height());
    cairo_fill(cr);
    color(theme.shadow_color, .30, .20, .40);
    cairo_rectangle(cr, 0, popup.height() - 2, width, 2);
    cairo_rectangle(cr, width - 2, 0, 2, popup.height());
    cairo_fill(cr);

    int left = 12;
    if (popup.image || !popup.icon.empty()) {
      if (auto* icon = popup.image ? popup.image.get() : shell.themed_icon_surface(popup.icon)) {
        const int icon_width = cairo_image_surface_get_width(icon);
        const int icon_height = cairo_image_surface_get_height(icon);
        if (icon_width > 0 && icon_height > 0) {
          cairo_save(cr);
          cairo_translate(cr, 11, 20);
          cairo_scale(cr, std::min(1.0, 40.0 / icon_width),
                      std::min(1.0, 40.0 / icon_height));
          cairo_set_source_surface(cr, icon, 0, 0);
          cairo_paint(cr);
          cairo_restore(cr);
          left = 60;
        }
      }
    }
    color(theme.text_color, .16, .09, .23);
    draw_text(cr, popup.summary, left, 10, std::max(1, width - left - 34), 23, true, theme);
    color(theme.text_color, .19, .18, .22);
    draw_text(cr, popup.body, left, 34, std::max(1, width - left - 12), 46, false, theme);
    // Explicit dismissal remains available even when the body invokes an action.
    draw_text(cr, "×", width - 26, 8, 20, 24, true, theme);
    if (popup.urgency == 2) {
      cairo_set_source_rgb(cr, .75, .15, .12);
      cairo_rectangle(cr, 3, 3, 4, popup.height() - 6);
      cairo_fill(cr);
    }
    const int button_width = (width - 24) / 2;
    for (std::size_t i = 0; i < popup.actions.size(); ++i) {
      const int x = 12 + int(i % 2) * button_width;
      const int y = popup_height + int(i / 2) * 28;
      color(theme.shadow_color, .30, .20, .40);
      cairo_rectangle(cr, x, y, button_width - 4, 24);
      cairo_stroke(cr);
      color(theme.text_color, .16, .09, .23);
      draw_text(cr, popup.actions[i].second, x + 5, y + 3, button_width - 14, 20, false, theme);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    xcb_change_window_attributes(connection, popup.window, XCB_CW_BACK_PIXMAP, &pixmap);
    const std::uint32_t dimensions[] = {std::uint32_t(width), std::uint32_t(popup.height())};
    xcb_configure_window(connection, popup.window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, dimensions);
    xcb_clear_area(connection, 0, popup.window, 0, 0, 0, 0);
    if (popup.pixmap) xcb_free_pixmap(connection, popup.pixmap);
    popup.pixmap = pixmap;
  }

  std::uint32_t notify(std::uint32_t replaces, const char* icon, const char* summary,
                       const char* body, gchar** actions, GVariant* hints, int timeout) {
    const auto found = replaces ? std::ranges::find(popups, replaces, &Popup::id) : popups.end();
    Popup* existing = found == popups.end() ? nullptr : &*found;
    if (!existing && static_cast<int>(popups.size()) >= max_popups) close(popups.front().id, 4);
    std::uint32_t id = existing ? existing->id : next_id++;
    if (!id) id = next_id++;
    if (!existing) {
      Popup popup;
      popup.id = id;
      popup.window = xcb_generate_id(connection);
      const int width = std::min(popup_width, std::max(1, int(screen->width_in_pixels) - 2 * gap));
      const std::uint32_t attributes[] = {screen->white_pixel, 1,
          XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS};
      const auto cookie = xcb_create_window_checked(
          connection, screen->root_depth, popup.window, screen->root, 0, 0,
          width, popup_height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, attributes);
      if (auto* error = xcb_request_check(connection, cookie)) {
        std::free(error);
        return id;
      }
      popups.push_back(std::move(popup));
      existing = &popups.back();
      reposition();

    }
    existing->summary = summary ? summary : "";
    existing->body = plain_body(body);
    existing->icon = icon_path(icon);
    existing->image = hint_image(hints, "image-data");
    if (!existing->image) existing->image = hint_image(hints, "image_data");
    const char* image_path = nullptr;
    if (!existing->image && (g_variant_lookup(hints, "image-path", "&s", &image_path) ||
                            g_variant_lookup(hints, "image_path", "&s", &image_path)))
      existing->icon = icon_path(image_path);
    if (!existing->image && existing->icon.empty()) existing->image = hint_image(hints, "icon_data");
    existing->actions.clear();
    existing->default_action.clear();
    for (unsigned i = 0; actions && actions[i] && actions[i + 1]; i += 2) {
      if (g_str_equal(actions[i], "default")) existing->default_action = actions[i];
      else existing->actions.emplace_back(actions[i], actions[i + 1]);
    }
    guchar urgency = 1;
    g_variant_lookup(hints, "urgency", "y", &urgency);
    existing->urgency = std::min(unsigned(urgency), 2u);
    gboolean resident = false;
    g_variant_lookup(hints, "resident", "b", &resident);
    existing->resident = resident;
    existing->expires = timeout != 0 && existing->urgency != 2;
    existing->deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(timeout < 0 ? default_timeout : timeout);
    reposition();
    paint(*existing);
    xcb_map_window(connection, existing->window);
    xcb_flush(connection);
    return id;
  }

  static void method(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                     const gchar* name, GVariant* parameters, GDBusMethodInvocation* invocation,
                     gpointer data) {
    auto& self = *static_cast<Impl*>(data);
    if (g_str_equal(name, "GetCapabilities")) {
      const char* capabilities[] = {"body", "icon-static", "actions", nullptr};
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", capabilities));
    } else if (g_str_equal(name, "GetServerInformation")) {
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(ssss)", "Midnight Workspace", "MidnightBSD",
                        MIDNIGHT_WM_VERSION, "1.3"));
    } else if (g_str_equal(name, "Notify")) {
      const char *app, *icon, *summary, *body;
      guint replaces;
      gchar** actions;
      GVariant* hints;
      gint timeout;
      g_variant_get(parameters, "(&su&s&s&s^as@a{sv}i)", &app, &replaces, &icon,
                    &summary, &body, &actions, &hints, &timeout);
      (void)app;
      const auto count = g_strv_length(actions);
      if (count % 2 || count > 18) {
        g_strfreev(actions);
        g_variant_unref(hints);
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.freedesktop.DBus.Error.InvalidArgs", "Expected at most nine action pairs");
        return;
      }
      const auto id = self.notify(replaces, icon, summary, body, actions, hints, timeout);
      g_strfreev(actions);
      g_variant_unref(hints);
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
    } else if (g_str_equal(name, "CloseNotification")) {
      guint id;
      g_variant_get(parameters, "(u)", &id);
      if (self.close(id, 3)) g_dbus_method_invocation_return_value(invocation, nullptr);
      else g_dbus_method_invocation_return_dbus_error(
          invocation, "org.freedesktop.Notifications.Error.InvalidId", "Unknown notification ID");
    }
  }
};

Notifications::Notifications(xcb_connection_t* connection, xcb_screen_t* screen, Shell& shell)
    : impl_(std::make_unique<Impl>(connection, screen, shell)) {}
Notifications::~Notifications() = default;

bool Notifications::initialize() {
  auto& self = *impl_;
  GError* error = nullptr;
  self.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!self.bus) { if (error) g_error_free(error); return false; }
  self.introspection = g_dbus_node_info_new_for_xml(interface_xml, &error);
  if (!self.introspection) { if (error) g_error_free(error); return false; }
  static const GDBusInterfaceVTable methods{Impl::method, nullptr, nullptr, {nullptr}};
  self.registration = g_dbus_connection_register_object(
      self.bus, path, self.introspection->interfaces[0], &methods, &self, nullptr, &error);
  if (!self.registration) { if (error) g_error_free(error); return false; }
  auto* reply = g_dbus_connection_call_sync(
      self.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "RequestName", g_variant_new("(su)", service, 4u), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
  if (!reply) {
    if (error) g_error_free(error);
    g_dbus_connection_unregister_object(self.bus, self.registration);
    self.registration = 0;
    return false;
  }
  guint result = 0;
  g_variant_get(reply, "(u)", &result);
  g_variant_unref(reply);
  self.owns_name = result == 1 || result == 4;
  if (!self.owns_name && self.registration) {
    g_dbus_connection_unregister_object(self.bus, self.registration);
    self.registration = 0;
  }
  return self.owns_name;
}

bool Notifications::handle(xcb_generic_event_t* event) {
  auto& self = *impl_;
  const auto type = event->response_type & ~0x80;
  if (type == XCB_EXPOSE) {
    const auto* exposed = reinterpret_cast<xcb_expose_event_t*>(event);
    const auto it = std::ranges::find(self.popups, exposed->window, &Impl::Popup::window);
    if (it != self.popups.end()) { if (!exposed->count && !it->pixmap) self.paint(*it); return true; }
  } else if (type == XCB_BUTTON_PRESS) {
    const auto* pressed = reinterpret_cast<xcb_button_press_event_t*>(event);
    const auto it = std::ranges::find(self.popups, pressed->event, &Impl::Popup::window);
    if (it != self.popups.end()) {
      if (pressed->detail != 1) return true;
      const int width = std::min(popup_width, std::max(1, int(self.screen->width_in_pixels) - 2 * gap));
      const int x = pressed->event_x, y = pressed->event_y;
      if (x >= width - 30 && y < 32) { self.close(it->id, 2); return true; }
      std::string action;
      if (y >= popup_height && x >= 12 && x < width - 12) {
        const int button_width = (width - 24) / 2;
        const auto index = std::size_t((y - popup_height) / 28 * 2 + (x - 12) / button_width);
        if (index < it->actions.size()) action = it->actions[index].first;
      } else if (y < popup_height) action = it->default_action;
      if (!action.empty()) {
        g_dbus_connection_emit_signal(self.bus, nullptr, path, service, "ActionInvoked",
                                      g_variant_new("(us)", it->id, action.c_str()), nullptr);
        if (!it->resident) self.close(it->id, 2);
      } else if (y < popup_height) self.close(it->id, 2);
      return true;
    }
  }
  return false;
}

void Notifications::refresh() {
  impl_->reposition();
  for (auto& popup : impl_->popups) impl_->paint(popup);
}

void Notifications::tick() {
  auto& self = *impl_;
  const auto now = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < self.popups.size();) {
    if (self.popups[i].expires && self.popups[i].deadline <= now)
      self.close(self.popups[i].id, 1);
    else ++i;
  }
}
}  // namespace midnight
