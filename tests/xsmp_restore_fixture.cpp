// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include <X11/SM/SMlib.h>
#include <gio/gio.h>
#include <xcb/xcb.h>
#include <map>
#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <poll.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping{};
void stop(int) { stopping = 1; }
void ignore_io(IceConn) {}
struct Client { std::string id, previous, output, literal, executable; bool complete{}, windows{}; std::map<std::string, xcb_window_t> handles; };
void save(SmcConn connection, SmPointer data, int, Bool, int, Bool) {
  auto& client = *static_cast<Client*>(data);
  std::vector<std::string> arguments{client.executable, "--output", client.output,
    "--literal", client.literal, "--sm-client-id", client.id};
  if (client.windows) { arguments.push_back("--windows"); arguments.push_back("yes"); }
  std::vector<SmPropValue> values;
  for (auto& argument : arguments) values.push_back({static_cast<int>(argument.size()), argument.data()});
  auto directory = std::filesystem::current_path().string();
  SmPropValue program{static_cast<int>(client.executable.size()), client.executable.data()};
  SmPropValue cwd{static_cast<int>(directory.size()), directory.data()};
  SmProp name{const_cast<char*>(SmProgram), const_cast<char*>(SmARRAY8), 1, &program};
  SmProp command{const_cast<char*>(SmRestartCommand), const_cast<char*>(SmLISTofARRAY8), static_cast<int>(values.size()), values.data()};
  SmProp current{const_cast<char*>(SmCurrentDirectory), const_cast<char*>(SmARRAY8), 1, &cwd};
  std::array<SmProp*, 3> props{&name, &command, &current};
  SmcSetProperties(connection, props.size(), props.data());
  SmcSaveYourselfDone(connection, True);
}
void complete(SmcConn, SmPointer data) { static_cast<Client*>(data)->complete = true; }
void noop(SmcConn, SmPointer) {}
}
int main(int argc, char** argv) {
  Client client;
  client.executable = std::filesystem::canonical(argv[0]).string();
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::string_view(argv[i]) == "--output") client.output = argv[i + 1];
    else if (std::string_view(argv[i]) == "--literal") client.literal = argv[i + 1];
    else if (std::string_view(argv[i]) == "--windows") client.windows = std::string_view(argv[i + 1]) == "yes";
    else if (std::string_view(argv[i]) == "--sm-client-id") client.previous = argv[i + 1];
    else return 2;
  }
  if (client.output.empty()) return 2;
  IceSetIOErrorHandler(ignore_io);
  std::signal(SIGTERM, stop);
  std::signal(SIGINT, stop);
  SmcCallbacks callbacks{};
  callbacks.save_yourself = {save, &client}; callbacks.save_complete = {complete, &client};
  callbacks.die = {noop, &client}; callbacks.shutdown_cancelled = {noop, &client};
  char error[256]{}, *id{};
  auto connection = SmcOpenConnection(nullptr, nullptr, SmProtoMajor, SmProtoMinor,
    SmcSaveYourselfProcMask | SmcSaveCompleteProcMask | SmcDieProcMask | SmcShutdownCancelledProcMask,
    &callbacks, client.previous.empty() ? nullptr : client.previous.c_str(), &id, sizeof(error), error);
  if (!connection || !id) return 1;
  client.id = id; std::free(id);
  xcb_connection_t* display{};
  if (client.windows) {
    display = xcb_connect(nullptr, nullptr);
    if (!display || xcb_connection_has_error(display)) return 1;
    auto* screen = xcb_setup_roots_iterator(xcb_get_setup(display)).data;
    const auto atom = [&](const char* name) {
      auto* reply = xcb_intern_atom_reply(display, xcb_intern_atom(display, 0, std::strlen(name), name), nullptr);
      const auto result = reply ? reply->atom : XCB_ATOM_NONE; std::free(reply); return result;
    };
    const auto leader = xcb_generate_id(display);
    xcb_create_window(display, screen->root_depth, leader, screen->root, 0, 0, 1, 1, 0,
      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
    xcb_change_property(display, XCB_PROP_MODE_REPLACE, leader, atom("SM_CLIENT_ID"), XCB_ATOM_STRING, 8, client.id.size(), client.id.data());
    int offset{};
    for (const char* name : {"normal", "shade", "max", "full", "partial", "tiny", "roleless", "duplicate1", "duplicate2", "bad-role"}) {
      auto window = xcb_generate_id(display);
      xcb_create_window(display, screen->root_depth, window, screen->root, 30 + offset, 60 + offset,
        320, 220, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
      offset += 6;
      std::string role = name;
      if (role.starts_with("duplicate")) role = "duplicate";
      if (role == "roleless") role.clear();
      if (role == "bad-role") role.assign(4097, 'x');
      xcb_change_property(display, XCB_PROP_MODE_REPLACE, window, atom("WM_CLIENT_LEADER"), XCB_ATOM_WINDOW, 32, 1, &leader);
      xcb_change_property(display, XCB_PROP_MODE_REPLACE, window, atom("WM_WINDOW_ROLE"), XCB_ATOM_STRING, 8, role.size(), role.data());
      xcb_change_property(display, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, std::strlen(name), name);
      if (std::string_view(name) == "normal")
        xcb_change_property(display, XCB_PROP_MODE_REPLACE, window, atom("SM_CLIENT_ID"), XCB_ATOM_STRING, 8, client.id.size(), client.id.data());
      xcb_map_window(display, window);
      client.handles[name] = window;
    }
    xcb_flush(display);
  }
  bool recorded{};
  while (!stopping) {
    pollfd fd{IceConnectionNumber(SmcGetIceConnection(connection)), POLLIN, 0};
    if (poll(&fd, 1, 100) > 0 && IceProcessMessages(SmcGetIceConnection(connection), nullptr, nullptr) != IceProcessMessagesSuccess) break;
    if (client.complete && !recorded) {
      auto* keys = g_key_file_new();
      for (const auto& pair : {std::pair{"id", client.id}, {"previous", client.previous},
           {"literal", client.literal}, {"cwd", std::filesystem::current_path().string()}})
        g_key_file_set_string(keys, "Client", pair.first, pair.second.c_str());
      for (const char* name : {"SESSION_MANAGER", "ICEAUTHORITY", "SSH_AUTH_SOCK", "DBUS_SESSION_BUS_ADDRESS", "MIDNIGHT_WM_RESTORE_PLAN", "MIDNIGHT_WM_RESTORE_MANAGER", "MIDNIGHT_WM_WINDOW_FD", "MIDNIGHT_WM_WINDOW_REPORT"})
        g_key_file_set_string(keys, "Client", name, g_getenv(name) ? g_getenv(name) : "");
      for (const auto& [name, window] : client.handles) g_key_file_set_uint64(keys, "Windows", name.c_str(), window);
      GError* bus_error{};
      auto* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &bus_error);
      g_key_file_set_boolean(keys, "Client", "bus_reachable", bus != nullptr);
      if (bus) g_object_unref(bus);
      if (bus_error) g_error_free(bus_error);
      const auto path = std::filesystem::path(client.output) / (std::to_string(getpid()) + ".ini");
      const bool written = g_key_file_save_to_file(keys, path.c_str(), nullptr);
      g_key_file_unref(keys);
      if (!written) return 1;
      recorded = true;
    }
  }
  if (display) xcb_disconnect(display);
  SmcCloseConnection(connection, 0, nullptr);
  return 0;
}
