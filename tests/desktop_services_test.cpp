// Copyright (c) 2026 Lucas Holt. All rights reserved.
#include "desktop_services.hpp"
#include <gio/gio.h>
#include <xcb/screensaver.h>
#include <cstdlib>
#include <memory>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
namespace {
void pump() {
  for (int i = 0; i < 100; ++i) {
    while (g_main_context_iteration(nullptr, false)) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
GDBusConnection* client() {
  return g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
    static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
}
GVariant* call(GDBusConnection* bus, const char* destination, const char* path, const char* iface, const char* method, GVariant* parameters) {
  struct Reply { bool done{}; GVariant* value{}; } reply;
  g_dbus_connection_call(bus, destination, path, iface, method, parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
    [](GObject* source, GAsyncResult* result, gpointer data) {
      auto& reply = *static_cast<Reply*>(data);
      reply.value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, nullptr);
      reply.done = true;
    }, &reply);
  while (!reply.done) g_main_context_iteration(nullptr, true);
  return reply.value;
}
bool check(bool result, const char* message) {
  if (!result) std::cerr << message << '\n';
  return result;
}
}
int main(int argc, char** argv) {
  const bool x11 = argc > 1 && std::string(argv[1]) == "x11";
  const bool existing = argc > 1 && std::string(argv[1]) == "existing";
  auto* prior = existing ? client() : nullptr;
  if (existing) {
    if (!prior) return 1;
    auto* acquired = call(prior, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.ScreenSaver", 4U));
    if (!acquired) return 1;
    g_variant_unref(acquired);
  }
  std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection(x11 ? xcb_connect(nullptr, nullptr) : nullptr, xcb_disconnect);
  if (x11 && xcb_connection_has_error(connection.get())) return 1;
  midnight::ThemeConfig theme;
  midnight::DesktopServices service(connection.get(), theme);
  if (!service.initialize()) return 1;
  pump();
  if (existing) {
    auto* owner = call(prior, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
        g_variant_new("(s)", "org.freedesktop.ScreenSaver"));
    const char* actual{};
    if (owner) g_variant_get(owner, "(&s)", &actual);
    const bool kept = actual && std::string(actual) == g_dbus_connection_get_unique_name(prior);
    if (owner) g_variant_unref(owner);
    g_dbus_connection_close_sync(prior, nullptr, nullptr);
    g_object_unref(prior);
    return check(kept, "existing ScreenSaver provider was replaced") ? 0 : 1;
  }
  auto* first = client();
  auto* second = client();
  if (!first || !second) return 1;
  bool valid = true;
  constexpr auto name = "org.freedesktop.impl.portal.desktop.midnight";
  constexpr auto path = "/org/freedesktop/portal/desktop";
  constexpr auto iface = "org.freedesktop.impl.portal.Settings";
  auto* result = call(first, name, path, iface, "Read", g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"));
  GVariant* value{};
  if (result) { g_variant_get(result, "(v)", &value); }
  valid &= check(value && g_variant_get_uint32(value) == 2, "light setting missing");
  if (value) g_variant_unref(value);
  if (result) g_variant_unref(result);
  unsigned changes{};
  const auto subscription = g_dbus_connection_signal_subscribe(first, name, iface, "SettingChanged", path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
    [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) { ++*static_cast<unsigned*>(data); }, &changes, nullptr);
  pump();
  theme.dark_mode = true;
  theme.accent_color = {.1, .2, .3};
  service.refresh(theme);
  pump();
  valid &= check(changes == 2, "live appearance signals missing");
  const char* patterns[] = {"org.freedesktop.*", nullptr};
  result = call(first, name, path, iface, "ReadAll", g_variant_new("(^as)", patterns));
  valid &= check(result && g_variant_is_of_type(result, G_VARIANT_TYPE("(a{sa{sv}})")), "ReadAll type mismatch");
  if (result) {
    auto* outer = g_variant_get_child_value(result, 0);
    auto* dict = g_variant_lookup_value(outer, "org.freedesktop.appearance", G_VARIANT_TYPE("a{sv}"));
    guint32 scheme{};
    valid &= check(dict && g_variant_lookup(dict, "color-scheme", "u", &scheme) && scheme == 1, "ReadAll failed namespace wildcard or dark change");
    if (dict) g_variant_unref(dict);
    g_variant_unref(outer);
    g_variant_unref(result);
  }
  result = call(first, name, path, iface, "Read", g_variant_new("(ss)", "org.freedesktop.appearance", "unknown"));
  valid &= check(!result, "unknown setting accepted");
  if (result) g_variant_unref(result);
  constexpr auto idle = "org.freedesktop.ScreenSaver";
  constexpr auto idlepath = "/org/freedesktop/ScreenSaver";
  result = call(first, idle, idlepath, idle, "Inhibit", g_variant_new("(ss)", "test", "video"));
  guint32 cookie{};
  if (result) { g_variant_get(result, "(u)", &cookie); g_variant_unref(result); }
  valid &= check(cookie && service.inhibitor_count() == 1, "inhibitor not created");
  if (x11) {
    auto* c = connection.get();
    const auto* screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    // A short idle timeout must not blank while inhibited. Explicit
    // force/lock requests intentionally remain effective.
    xcb_set_screen_saver(c, 1, 0, XCB_BLANKING_PREFERRED, XCB_EXPOSURES_ALLOWED);
    xcb_force_screen_saver(c, XCB_SCREEN_SAVER_RESET);
    xcb_flush(c);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto* status = xcb_screensaver_query_info_reply(c, xcb_screensaver_query_info(c, screen->root), nullptr);
    valid &= check(status && status->state == XCB_SCREENSAVER_STATE_OFF, "inhibition did not suspend X11 screen saver");
    std::free(status);
    service.tick();
    pump();
  }
  result = call(second, idle, idlepath, idle, "UnInhibit", g_variant_new("(u)", cookie));
  valid &= check(!result && service.inhibitor_count() == 1, "another client removed inhibitor");
  if (result) g_variant_unref(result);
  result = call(first, idle, "/ScreenSaver", idle, "UnInhibit", g_variant_new("(u)", cookie));
  valid &= check(result && service.inhibitor_count() == 0, "inhibitor not released via alias");
  if (result) g_variant_unref(result);
  if (x11) {
    auto* c = connection.get();
    const auto* screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_force_screen_saver(c, XCB_SCREEN_SAVER_RESET);
    xcb_flush(c);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto* status = xcb_screensaver_query_info_reply(c, xcb_screensaver_query_info(c, screen->root), nullptr);
    valid &= check(status && status->state == XCB_SCREENSAVER_STATE_ON, "X11 screen saver remained suspended after release");
    std::free(status);
    xcb_force_screen_saver(c, XCB_SCREEN_SAVER_RESET);
    xcb_flush(c);
  }
  result = call(second, idle, idlepath, idle, "Inhibit", g_variant_new("(ss)", "test", "client exit"));
  if (result) g_variant_unref(result);
  g_dbus_connection_close_sync(second, nullptr, nullptr);
  pump();
  valid &= check(service.inhibitor_count() == 0, "disconnected client inhibitor leaked");
  g_dbus_connection_signal_unsubscribe(first, subscription);
  g_dbus_connection_close_sync(first, nullptr, nullptr);
  g_object_unref(first);
  g_object_unref(second);
  return valid ? 0 : 1;
}
