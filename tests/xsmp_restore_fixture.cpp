// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include <X11/SM/SMlib.h>
#include <gio/gio.h>
#include <array>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <poll.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping{};
void stop(int) { stopping = 1; }
void ignore_io(IceConn) {}
struct Client { std::string id, previous, output, literal, executable; bool complete{}; };
void save(SmcConn connection, SmPointer data, int, Bool, int, Bool) {
  auto& client = *static_cast<Client*>(data);
  std::vector<std::string> arguments{client.executable, "--output", client.output,
    "--literal", client.literal, "--sm-client-id", client.id};
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
  bool recorded{};
  while (!stopping) {
    pollfd fd{IceConnectionNumber(SmcGetIceConnection(connection)), POLLIN, 0};
    if (poll(&fd, 1, 100) > 0 && IceProcessMessages(SmcGetIceConnection(connection), nullptr, nullptr) != IceProcessMessagesSuccess) break;
    if (client.complete && !recorded) {
      auto* keys = g_key_file_new();
      for (const auto& pair : {std::pair{"id", client.id}, {"previous", client.previous},
           {"literal", client.literal}, {"cwd", std::filesystem::current_path().string()}})
        g_key_file_set_string(keys, "Client", pair.first, pair.second.c_str());
      for (const char* name : {"SESSION_MANAGER", "ICEAUTHORITY", "SSH_AUTH_SOCK", "DBUS_SESSION_BUS_ADDRESS", "MIDNIGHT_WM_RESTORE_PLAN", "MIDNIGHT_WM_RESTORE_MANAGER"})
        g_key_file_set_string(keys, "Client", name, g_getenv(name) ? g_getenv(name) : "");
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
  SmcCloseConnection(connection, 0, nullptr);
  return 0;
}
