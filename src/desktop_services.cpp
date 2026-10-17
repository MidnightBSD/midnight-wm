// Copyright (c) 2026 Lucas Holt. All rights reserved.
#include "desktop_services.hpp"
#include <gio/gio.h>
#include <xcb/screensaver.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>

namespace midnight {
namespace {
constexpr auto settings_name = "org.freedesktop.impl.portal.desktop.midnight";
constexpr auto settings_path = "/org/freedesktop/portal/desktop";
constexpr auto settings_iface = "org.freedesktop.impl.portal.Settings";
constexpr auto idle_name = "org.freedesktop.ScreenSaver";
constexpr auto appearance = "org.freedesktop.appearance";
constexpr auto xml = R"xml(<node>
<interface name="org.freedesktop.impl.portal.Settings">
<property name="version" type="u" access="read"/>
<method name="Read"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
<method name="ReadAll"><arg type="as" direction="in"/><arg type="a{sa{sv}}" direction="out"/></method>
<signal name="SettingChanged"><arg type="s"/><arg type="s"/><arg type="v"/></signal>
</interface>
<interface name="org.freedesktop.ScreenSaver">
<method name="Inhibit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="u" direction="out"/></method>
<method name="UnInhibit"><arg type="u" direction="in"/></method>
</interface></node>)xml";
}
struct DesktopServices::Impl {
  xcb_connection_t* xcb;
  ThemeConfig theme;
  GDBusConnection* bus{};
  GDBusNodeInfo* info{};
  guint settings_object{}, idle_object{}, idle_alias{}, settings_owner{}, idle_owner{}, watch{};
  bool owns_idle{}, suspended{};
  xcb_atom_t saver_status{}, saver_lock{}, saver_auth{};
  std::uint32_t next_cookie{};
  std::map<std::uint32_t, std::string> inhibitors;
  std::chrono::steady_clock::time_point pulse_time{};
  struct Pulse {
    GSubprocess* process{};
    bool done{};
    ~Pulse() { if (process) g_object_unref(process); }
  };
  std::shared_ptr<Pulse> pulse;
  Impl(xcb_connection_t* connection, const ThemeConfig& value) : xcb(connection), theme(value) {}
  ~Impl() {
    if (pulse && !pulse->done) g_subprocess_force_exit(pulse->process);
    suspend(false);
    if (settings_owner) g_bus_unown_name(settings_owner);
    if (idle_owner) g_bus_unown_name(idle_owner);
    if (bus) {
      if (watch) g_dbus_connection_signal_unsubscribe(bus, watch);
      for (auto id : {settings_object, idle_object, idle_alias})
        if (id) g_dbus_connection_unregister_object(bus, id);
      g_object_unref(bus);
    }
    if (info) g_dbus_node_info_unref(info);
  }
  void suspend(bool value) {
    if (suspended == value) return;
    suspended = value;
    if (xcb) {
      const auto* extension = xcb_get_extension_data(xcb, &xcb_screensaver_id);
      if (extension && extension->present) xcb_screensaver_suspend(xcb, value);
      xcb_flush(xcb);
    }
    pulse_time = {};
  }
  GVariant* setting(std::string_view key) const {
    if (key == "color-scheme") return g_variant_new_uint32(theme.dark_mode ? 1 : 2);
    if (key == "accent-color") return g_variant_new("(ddd)", theme.accent_color[0], theme.accent_color[1], theme.accent_color[2]);
    if (key == "contrast") return g_variant_new_uint32(0);
    return nullptr;
  }
  void settings(const char* method, GVariant* parameters, GDBusMethodInvocation* invocation) {
    if (std::string_view(method) == "Read") {
      const char *space, *key;
      g_variant_get(parameters, "(&s&s)", &space, &key);
      auto* value = std::string_view(space) == appearance ? setting(key) : nullptr;
      if (value) g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
      else g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.portal.Error.NotFound", "Unknown setting");
    } else {
      gchar** filters{};
      g_variant_get(parameters, "(^as)", &filters);
      bool matches = !filters[0];
      for (auto** filter = filters; *filter; ++filter) {
        const std::string_view pattern(*filter);
        matches |= pattern.empty() || pattern == appearance ||
            (pattern.ends_with(".*") && std::string_view(appearance).starts_with(pattern.substr(0, pattern.size() - 1)));
      }
      g_strfreev(filters);
      GVariantBuilder result;
      g_variant_builder_init(&result, G_VARIANT_TYPE("a{sa{sv}}"));
      if (matches) {
        GVariantBuilder values;
        g_variant_builder_init(&values, G_VARIANT_TYPE("a{sv}"));
        for (auto key : {"color-scheme", "accent-color", "contrast"})
          g_variant_builder_add(&values, "{sv}", key, setting(key));
        g_variant_builder_add(&result, "{sa{sv}}", appearance, &values);
      }
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sa{sv}})", &result));
    }
  }
  void idle(const char* sender, const char* method, GVariant* parameters, GDBusMethodInvocation* invocation) {
    if (!owns_idle) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.NotSupported", "Another idle inhibitor is active");
      return;
    }
    if (std::string_view(method) == "Inhibit") {
      // Bound memory and protocol work even for an uncooperative session peer.
      if (inhibitors.size() >= 1024) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.LimitsExceeded", "Too many inhibitors");
        return;
      }
      do { ++next_cookie; } while (!next_cookie || inhibitors.contains(next_cookie));
      inhibitors.emplace(next_cookie, sender);
      suspend(true);
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", next_cookie));
    } else {
      guint32 cookie;
      g_variant_get(parameters, "(u)", &cookie);
      auto found = inhibitors.find(cookie);
      if (found == inhibitors.end() || found->second != sender) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.InvalidArgs", "Unknown inhibitor for this caller");
        return;
      }
      inhibitors.erase(found);
      suspend(!inhibitors.empty());
      g_dbus_method_invocation_return_value(invocation, nullptr);
    }
  }
  bool initialize() {
    if (xcb) {
      auto atom = [this](const char* name) {
        auto* reply = xcb_intern_atom_reply(xcb, xcb_intern_atom(xcb, false, std::char_traits<char>::length(name), name), nullptr);
        const auto value = reply ? reply->atom : XCB_ATOM_NONE;
        std::free(reply);
        return value;
      };
      saver_status = atom("_SCREENSAVER_STATUS");
      saver_lock = atom("LOCK");
      saver_auth = atom("AUTH");
    }
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return false;
    info = g_dbus_node_info_new_for_xml(xml, nullptr);
    static const GDBusInterfaceVTable vtable = {
      [](GDBusConnection*, const gchar* sender, const gchar*, const gchar* iface, const gchar* method,
         GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
        auto& self = *static_cast<Impl*>(data);
        if (std::string_view(iface) == settings_iface) self.settings(method, parameters, invocation);
        else self.idle(sender, method, parameters, invocation);
      },
      [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GError**, gpointer) -> GVariant* {
        return g_variant_new_uint32(2);
      }, nullptr, {nullptr}
    };
    settings_object = g_dbus_connection_register_object(bus, settings_path, info->interfaces[0], &vtable, this, nullptr, nullptr);
    idle_object = g_dbus_connection_register_object(bus, "/org/freedesktop/ScreenSaver", info->interfaces[1], &vtable, this, nullptr, nullptr);
    idle_alias = g_dbus_connection_register_object(bus, "/ScreenSaver", info->interfaces[1], &vtable, this, nullptr, nullptr);
    if (settings_object) settings_owner = g_bus_own_name_on_connection(bus, settings_name, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, nullptr, nullptr, this, nullptr);
    if (idle_object && idle_alias) idle_owner = g_bus_own_name_on_connection(bus, idle_name, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
      [](GDBusConnection*, const gchar*, gpointer data) { static_cast<Impl*>(data)->owns_idle = true; },
      [](GDBusConnection*, const gchar*, gpointer data) {
        auto& self = *static_cast<Impl*>(data);
        self.owns_idle = false;
        self.inhibitors.clear();
        self.suspend(false);
      }, this, nullptr);
    watch = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer data) {
          const char *name, *old_owner, *new_owner;
          g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
          if (*new_owner || *name != ':') return;
          auto& self = *static_cast<Impl*>(data);
          std::erase_if(self.inhibitors, [name](const auto& item) { return item.second == name; });
          self.suspend(!self.inhibitors.empty());
        }, this, nullptr);
    // Pass only the desktop/display variables needed by activated GUI backends.
    // Do not replace the user's portal configuration or restart an existing portal.
    GVariantBuilder environment;
    g_variant_builder_init(&environment, G_VARIANT_TYPE("a{ss}"));
    for (auto key : {"DISPLAY", "XAUTHORITY", "XDG_CURRENT_DESKTOP"})
      if (const auto* value = g_getenv(key)) g_variant_builder_add(&environment, "{ss}", key, value);
    g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "UpdateActivationEnvironment",
        g_variant_new("(a{ss})", &environment), nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr, nullptr);
    return settings_object && idle_object && idle_alias;
  }
  void tick() {
    if (!owns_idle || inhibitors.empty() || !xcb) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - pulse_time < std::chrono::seconds(15) || (pulse && !pulse->done)) return;
    pulse_time = now;
    // Do not repeatedly raise the password dialog after an explicit lock.
    const auto* screen = xcb_setup_roots_iterator(xcb_get_setup(xcb)).data;
    auto* status = xcb_get_property_reply(xcb, xcb_get_property(xcb, false, screen->root,
        saver_status, XCB_ATOM_INTEGER, 0, 1), nullptr);
    bool locked = false;
    if (status && status->format == 32 && xcb_get_property_value_length(status) >= 4) {
      const auto state = *static_cast<const xcb_atom_t*>(xcb_get_property_value(status));
      locked = state == saver_lock || state == saver_auth;
    }
    std::free(status);
    if (locked) return;
    // XScreenSaverSuspend covers the server's blanking/DPMS. XScreenSaver's
    // daemon has its own idle timer. Its deactivate command cannot unlock a
    // locked session; explicit locking is never inhibited.
    auto* process = g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                    nullptr, "xscreensaver-command", "-deactivate", nullptr);
    if (!process) return;
    pulse = std::make_shared<Pulse>();
    pulse->process = process;
    g_subprocess_wait_async(process, nullptr, [](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<std::shared_ptr<Pulse>> state(static_cast<std::shared_ptr<Pulse>*>(data));
      g_subprocess_wait_finish(G_SUBPROCESS(source), result, nullptr);
      (*state)->done = true;
    }, new std::shared_ptr<Pulse>(pulse));
  }
};
DesktopServices::DesktopServices(xcb_connection_t* connection, const ThemeConfig& theme) : impl_(std::make_unique<Impl>(connection, theme)) {}
DesktopServices::~DesktopServices() = default;
bool DesktopServices::initialize() { return impl_->initialize(); }
void DesktopServices::tick() { impl_->tick(); }
std::size_t DesktopServices::inhibitor_count() const { return impl_->inhibitors.size(); }
void DesktopServices::refresh(const ThemeConfig& theme) {
  const bool color_changed = impl_->theme.dark_mode != theme.dark_mode;
  const bool accent_changed = impl_->theme.accent_color != theme.accent_color;
  impl_->theme = theme;
  if (!impl_->bus || !impl_->settings_object) return;
  for (auto [changed, key] : {std::pair{color_changed, "color-scheme"}, std::pair{accent_changed, "accent-color"}})
    if (changed) g_dbus_connection_emit_signal(impl_->bus, nullptr, settings_path, settings_iface, "SettingChanged",
        g_variant_new("(ssv)", appearance, key, impl_->setting(key)), nullptr);
}
}
