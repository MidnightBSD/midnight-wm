#include "file_manager1.hpp"

#include <gio/gio.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {
struct Reply {
  GMainLoop* loop{};
  GVariant* value{};
  GError* error{};
};

void completed(GObject* source, GAsyncResult* result, gpointer user_data) {
  auto& reply = *static_cast<Reply*>(user_data);
  reply.value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &reply.error);
  g_main_loop_quit(reply.loop);
}

bool call(GDBusConnection* bus, const char* method, const char* uri, bool expect_success) {
  const gchar* uris[] = {uri, nullptr};
  Reply reply{.loop = g_main_loop_new(nullptr, FALSE)};
  g_dbus_connection_call(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
      "org.freedesktop.FileManager1", method, g_variant_new("(^ass)", uris, ""),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, completed, &reply);
  g_main_loop_run(reply.loop);
  const bool success = reply.value != nullptr;
  if (success != expect_success)
    std::cerr << method << " returned " << (reply.error ? reply.error->message : "success") << '\n';
  if (reply.value) g_variant_unref(reply.value);
  if (reply.error) g_error_free(reply.error);
  g_main_loop_unref(reply.loop);
  return success == expect_success;
}

bool object_registered(GDBusConnection* bus, const char* destination) {
  Reply reply{.loop = g_main_loop_new(nullptr, FALSE)};
  g_dbus_connection_call(bus, destination, "/org/freedesktop/FileManager1",
      "org.freedesktop.DBus.Introspectable", "Introspect", nullptr, G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, completed, &reply);
  g_main_loop_run(reply.loop);
  const gchar* xml = nullptr;
  if (reply.value) g_variant_get(reply.value, "(&s)", &xml);
  const bool registered = xml && g_strstr_len(xml, -1, "<interface name=\"org.freedesktop.FileManager1\"") != nullptr;
  if (reply.value) g_variant_unref(reply.value);
  const bool missing = reply.error &&
      g_dbus_error_is_remote_error(reply.error) &&
      g_strcmp0(g_dbus_error_get_remote_error(reply.error), "org.freedesktop.DBus.Error.UnknownObject") == 0;
  if (reply.error) g_error_free(reply.error);
  g_main_loop_unref(reply.loop);
  return reply.value ? registered : !missing;
}
} // namespace

int main() {
  GError* error = nullptr;
  const char* address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
  auto* competitor = address ? g_dbus_connection_new_for_address_sync(
      address, static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                 G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error) : nullptr;
  if (!competitor) {
    std::cerr << (error ? error->message : "Session bus unavailable") << '\n';
    if (error) g_error_free(error);
    return 1;
  }
  auto* owner_reply = g_dbus_connection_call_sync(
      competitor, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "RequestName", g_variant_new("(su)", "org.freedesktop.FileManager1", 4u),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
  if (!owner_reply) {
    std::cerr << (error ? error->message : "Could not reserve FileManager1 name") << '\n';
    if (error) g_error_free(error);
    g_object_unref(competitor);
    return 1;
  }
  guint owner_result = 0;
  g_variant_get(owner_reply, "(u)", &owner_result);
  g_variant_unref(owner_reply);
  if (owner_result != 1) {
    std::cerr << "Could not reserve FileManager1 name for regression check\n";
    g_object_unref(competitor);
    return 1;
  }
  midnight::FileManager1 rejected_service([](const auto&, const auto&) { return true; },
                                           [](const auto&) { return true; });
  if (rejected_service.initialize()) {
    std::cerr << "FileManager1 initialized while another process owned its name\n";
    g_object_unref(competitor);
    return 1;
  }
  auto* own_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  const char* own_name = own_bus ? g_dbus_connection_get_unique_name(own_bus) : nullptr;
  const bool unregistered = own_name && !object_registered(competitor, own_name);
  if (own_bus) g_object_unref(own_bus);
  if (error) { g_error_free(error); error = nullptr; }
  auto* release_reply = g_dbus_connection_call_sync(
      competitor, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "ReleaseName", g_variant_new("(s)", "org.freedesktop.FileManager1"),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
  if (release_reply) g_variant_unref(release_reply);
  g_object_unref(competitor);
  if (!unregistered || error) {
    std::cerr << "FileManager1 left its object registered after name acquisition failed\n";
    if (error) g_error_free(error);
    return 1;
  }

  std::vector<std::pair<std::filesystem::path, std::optional<std::filesystem::path>>> folders;
  std::vector<std::filesystem::path> properties;
  midnight::FileManager1 service(
      [&](const auto& folder, const auto& selected) {
        folders.emplace_back(folder, selected);
        return true;
      },
      [&](const auto& item) {
        properties.push_back(item);
        return true;
      });
  if (!service.initialize()) { std::cerr << "Could not own FileManager1\n"; return 1; }
  auto* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) {
    std::cerr << (error ? error->message : "Session bus unavailable") << '\n';
    if (error) g_error_free(error);
    return 1;
  }
  bool valid = call(bus, "ShowFolders", "file:///tmp", true) &&
               call(bus, "ShowItems", "file:///tmp/example%20file.txt", true) &&
               call(bus, "ShowItemProperties", "file:///tmp/example%20file.txt", true) &&
               call(bus, "ShowFolders", "https://example.org/folder", false);
  valid = valid && folders.size() == 2 && properties.size() == 1 &&
          folders[0].first == "/tmp" && !folders[0].second &&
          folders[1].first == "/tmp" &&
          folders[1].second == "/tmp/example file.txt" &&
          properties[0] == "/tmp/example file.txt";
  if (!valid) std::cerr << "FileManager1 callbacks did not match the URIs\n";
  g_object_unref(bus);
  return valid ? 0 : 1;
}
