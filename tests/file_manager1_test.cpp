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
} // namespace

int main() {
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
  GError* error = nullptr;
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
