#include "file_manager1.hpp"

#include <gio/gio.h>

#include <string>
#include <utility>
#include <vector>

namespace midnight {
namespace {
constexpr auto interface_xml = R"xml(
<node><interface name="org.freedesktop.FileManager1">
  <method name="ShowFolders"><arg name="URIs" type="as" direction="in"/>
    <arg name="StartupId" type="s" direction="in"/></method>
  <method name="ShowItems"><arg name="URIs" type="as" direction="in"/>
    <arg name="StartupId" type="s" direction="in"/></method>
  <method name="ShowItemProperties"><arg name="URIs" type="as" direction="in"/>
    <arg name="StartupId" type="s" direction="in"/></method>
</interface></node>)xml";

std::optional<std::filesystem::path> local_path(const char* uri) {
  if (!uri || !*uri || !g_str_has_prefix(uri, "file://")) return std::nullopt;
  auto* file = g_file_new_for_uri(uri);
  char* raw = g_file_get_path(file);
  std::optional<std::filesystem::path> result;
  if (raw && g_path_is_absolute(raw)) result = std::filesystem::path(raw).lexically_normal();
  g_free(raw);
  g_object_unref(file);
  return result;
}
} // namespace

struct FileManager1::Impl {
  ShowFolder show_folder;
  ShowProperties show_properties;
  std::string service_name;
  std::string object_path;
  GDBusConnection* bus{};
  GDBusNodeInfo* introspection{};
  guint registration{};
  bool owns_name{};

  Impl(ShowFolder folder, ShowProperties properties, std::string name, std::string path)
      : show_folder(std::move(folder)), show_properties(std::move(properties)),
        service_name(std::move(name)), object_path(std::move(path)) {}

  ~Impl() {
    if (registration) g_dbus_connection_unregister_object(bus, registration);
    if (owns_name && bus) {
      auto* reply = g_dbus_connection_call_sync(
          bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
          "ReleaseName", g_variant_new("(s)", service_name.c_str()), G_VARIANT_TYPE("(u)"),
          G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
      if (reply) g_variant_unref(reply);
    }
    if (introspection) g_dbus_node_info_unref(introspection);
    if (bus) g_object_unref(bus);
  }

  static void method(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                     const gchar* name, GVariant* parameters,
                     GDBusMethodInvocation* invocation, gpointer data) {
    auto& self = *static_cast<Impl*>(data);
    gchar** uris = nullptr;
    const gchar* startup_id = nullptr;
    g_variant_get(parameters, "(^as&s)", &uris, &startup_id);
    (void)startup_id; // The shell opens its own windows; startup tracking is handled separately.

    std::vector<std::filesystem::path> paths;
    for (gchar** uri = uris; uri && *uri; ++uri) {
      auto path = local_path(*uri);
      if (!path) {
        g_strfreev(uris);
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.freedesktop.FileManager1.Error.InvalidURI",
            "Only local file URIs are supported");
        return;
      }
      paths.push_back(std::move(*path));
    }
    g_strfreev(uris);

    bool success = true;
    if (g_str_equal(name, "ShowFolders")) {
      for (const auto& path : paths)
        success = self.show_folder(path, std::nullopt) && success;
    } else if (g_str_equal(name, "ShowItems")) {
      for (const auto& path : paths) {
        const auto parent = path.has_parent_path() ? path.parent_path() : path;
        success = self.show_folder(parent, path) && success;
      }
    } else if (g_str_equal(name, "ShowItemProperties")) {
      // Midnight currently has one Properties dialog. The first item is shown.
      if (!paths.empty()) success = self.show_properties(paths.front());
    } else {
      g_dbus_method_invocation_return_dbus_error(invocation,
          "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
      return;
    }
    if (success) g_dbus_method_invocation_return_value(invocation, nullptr);
    else g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.FileManager1.Error.Failed", "File or folder could not be shown");
  }
};

FileManager1::FileManager1(ShowFolder show_folder, ShowProperties show_properties,
                           std::string service_name, std::string object_path)
    : impl_(std::make_unique<Impl>(std::move(show_folder), std::move(show_properties),
                                   std::move(service_name), std::move(object_path))) {}

FileManager1::~FileManager1() = default;

bool FileManager1::initialize() {
  auto& self = *impl_;
  GError* error = nullptr;
  self.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!self.bus) { if (error) g_error_free(error); return false; }
  self.introspection = g_dbus_node_info_new_for_xml(interface_xml, &error);
  if (!self.introspection) { if (error) g_error_free(error); return false; }
  static const GDBusInterfaceVTable methods{Impl::method, nullptr, nullptr, {nullptr}};
  self.registration = g_dbus_connection_register_object(
      self.bus, self.object_path.c_str(), self.introspection->interfaces[0], &methods,
      &self, nullptr, &error);
  if (!self.registration) { if (error) g_error_free(error); return false; }
  auto* reply = g_dbus_connection_call_sync(
      self.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "RequestName", g_variant_new("(su)", self.service_name.c_str(), 4u), G_VARIANT_TYPE("(u)"),
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

} // namespace midnight
