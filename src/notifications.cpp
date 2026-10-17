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
}  // namespace

struct Notifications::Impl {
  struct Popup {
    std::uint32_t id{};
    xcb_window_t window{};
    std::string summary;
    std::string body;
    std::string icon;
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
    for (const auto& popup : popups) xcb_destroy_window(connection, popup.window);
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
    popups.erase(it);
    reposition();
    emit_closed(id, reason);
    return true;
  }

  void reposition() {
    const int width = std::min(popup_width, std::max(1, int(screen->width_in_pixels) - 2 * gap));
    const int x = std::max(0, int(screen->width_in_pixels) - width - gap);
    for (std::size_t i = 0; i < popups.size(); ++i) {
      const int y = gap + 34 + static_cast<int>(i) * (popup_height + gap);
      const std::uint32_t coordinates[] = {std::uint32_t(x), std::uint32_t(y)};
      xcb_configure_window(connection, popups[i].window,
                           XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coordinates);
    }
  }

  void paint(const Popup& popup) {
    auto* visual = root_visual(screen);
    if (!visual) return;
    const int width = std::min(popup_width, std::max(1, int(screen->width_in_pixels) - 2 * gap));
    auto* surface = cairo_xcb_surface_create(connection, popup.window, visual, width, popup_height);
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
    cairo_rectangle(cr, 0, 0, 2, popup_height);
    cairo_fill(cr);
    color(theme.shadow_color, .30, .20, .40);
    cairo_rectangle(cr, 0, popup_height - 2, width, 2);
    cairo_rectangle(cr, width - 2, 0, 2, popup_height);
    cairo_fill(cr);

    int left = 12;
    if (!popup.icon.empty()) {
      if (auto* icon = shell.themed_icon_surface(popup.icon)) {
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
    draw_text(cr, popup.summary, left, 10, std::max(1, width - left - 12), 23, true, theme);
    color(theme.text_color, .19, .18, .22);
    draw_text(cr, popup.body, left, 34, std::max(1, width - left - 12), 46, false, theme);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
  }

  std::uint32_t notify(std::uint32_t replaces, const char* icon, const char* summary,
                       const char* body, int timeout) {
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
      xcb_map_window(connection, existing->window);
    }
    existing->summary = summary ? summary : "";
    existing->body = plain_body(body);
    existing->icon = icon ? icon : "";
    existing->expires = timeout != 0;
    existing->deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(timeout < 0 ? default_timeout : timeout);
    paint(*existing);
    xcb_flush(connection);
    return id;
  }

  static void method(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                     const gchar* name, GVariant* parameters, GDBusMethodInvocation* invocation,
                     gpointer data) {
    auto& self = *static_cast<Impl*>(data);
    if (g_str_equal(name, "GetCapabilities")) {
      const char* capabilities[] = {"body", "icon-static", nullptr};
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
      g_strfreev(actions);
      g_variant_unref(hints);
      const auto id = self.notify(replaces, icon, summary, body, timeout);
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
  if (!reply) { if (error) g_error_free(error); return false; }
  guint result = 0;
  g_variant_get(reply, "(u)", &result);
  g_variant_unref(reply);
  self.owns_name = result == 1 || result == 4;
  return self.owns_name;
}

bool Notifications::handle(xcb_generic_event_t* event) {
  auto& self = *impl_;
  const auto type = event->response_type & ~0x80;
  if (type == XCB_EXPOSE) {
    const auto* exposed = reinterpret_cast<xcb_expose_event_t*>(event);
    const auto it = std::ranges::find(self.popups, exposed->window, &Impl::Popup::window);
    if (it != self.popups.end()) { if (!exposed->count) self.paint(*it); return true; }
  } else if (type == XCB_BUTTON_PRESS) {
    const auto* pressed = reinterpret_cast<xcb_button_press_event_t*>(event);
    const auto it = std::ranges::find(self.popups, pressed->event, &Impl::Popup::window);
    if (it != self.popups.end()) { self.close(it->id, 2); return true; }
  }
  return false;
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
