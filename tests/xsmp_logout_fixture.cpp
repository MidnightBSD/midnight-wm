// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include <X11/SM/SMlib.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdlib>
#include <poll.h>
#include <unistd.h>

namespace {
struct Client {
  std::filesystem::path root;
  std::string name, mode, executable, id;
  bool interacting{}, stopping{}, waiting{};
};
void event(const Client& client, const char* name) {
  std::ofstream(client.root / "events", std::ios::app) << client.name << ' ' << name << '\n';
}
void phase2(SmcConn connection, SmPointer data) {
  static_cast<Client*>(data)->waiting = false;
  event(*static_cast<Client*>(data), "phase2");
  SmcSaveYourselfDone(connection, True);
}
void interact(SmcConn, SmPointer data) {
  auto& client = *static_cast<Client*>(data);
  client.interacting = true;
  event(client, "interact");
}
void save(SmcConn connection, SmPointer data, int, Bool shutdown, int style, Bool fast) {
  auto& client = *static_cast<Client*>(data);
  std::string arguments[]{client.executable, client.root.string(), client.name};
  SmPropValue values[3];
  for (int i = 0; i < 3; ++i) values[i] = {int(arguments[i].size()), arguments[i].data()};
  SmProp command{const_cast<char*>(SmRestartCommand), const_cast<char*>(SmLISTofARRAY8), 3, values};
  SmPropValue program{int(client.executable.size()), client.executable.data()};
  SmProp name{const_cast<char*>(SmProgram), const_cast<char*>(SmARRAY8), 1, &program};
  SmProp* props[]{&command, &name};
  SmcSetProperties(connection, 2, props);
  if (!shutdown) { SmcSaveYourselfDone(connection, True); return; }
  client.waiting = true;
  if (style != SmInteractStyleAny || fast) std::exit(3);
  event(client, "shutdown");
  std::ifstream(client.root / (client.name + ".mode")) >> client.mode;
  if (client.mode == "hang") return;
  if (client.mode == "disconnect") { SmcCloseConnection(connection, 0, nullptr); std::exit(0); }
  if (client.mode == "prompt" || client.mode == "cancel") {
    if (!SmcInteractRequest(connection, SmDialogNormal, interact, data)) std::exit(4);
  } else if (client.mode == "phase2") {
    if (!SmcRequestSaveYourselfPhase2(connection, phase2, data)) std::exit(5);
  } else { client.waiting = false; SmcSaveYourselfDone(connection, client.mode != "fail"); }
}
void complete(SmcConn, SmPointer data) { event(*static_cast<Client*>(data), "complete"); }
void die(SmcConn, SmPointer data) {
  auto& client = *static_cast<Client*>(data);
  event(client, "die");
  client.stopping = client.mode != "ignore-die";
}
void cancelled(SmcConn connection, SmPointer data) {
  auto& client = *static_cast<Client*>(data);
  event(client, "cancelled");
  client.interacting = false;
  if (client.mode != "hang" && client.waiting) { client.waiting = false; SmcSaveYourselfDone(connection, True); }
}
void io_error(IceConn) {}
}
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  Client client;
  client.root = argv[1]; client.name = argv[2];
  client.executable = std::filesystem::canonical(argv[0]).string();
  IceSetIOErrorHandler(io_error);
  SmcCallbacks callbacks{};
  callbacks.save_yourself = {save, &client}; callbacks.save_complete = {complete, &client};
  callbacks.die = {die, &client}; callbacks.shutdown_cancelled = {cancelled, &client};
  char error[256]{}, *id{};
  auto connection = SmcOpenConnection(nullptr, nullptr, SmProtoMajor, SmProtoMinor,
    SmcSaveYourselfProcMask | SmcSaveCompleteProcMask | SmcDieProcMask | SmcShutdownCancelledProcMask,
    &callbacks, nullptr, &id, sizeof(error), error);
  if (!connection || !id) return 1;
  client.id = id; std::free(id);
  while (!client.stopping) {
    pollfd fd{IceConnectionNumber(SmcGetIceConnection(connection)), POLLIN, 0};
    if (poll(&fd, 1, 20) > 0 && IceProcessMessages(SmcGetIceConnection(connection), nullptr, nullptr) != IceProcessMessagesSuccess) break;
    if (client.interacting && std::filesystem::exists(client.root / (client.name + ".release"))) {
      std::filesystem::remove(client.root / (client.name + ".release"));
      client.interacting = false;
      event(client, "interact-done");
      SmcInteractDone(connection, client.mode == "cancel");
      client.waiting = false;
      SmcSaveYourselfDone(connection, True);
    }
  }
  SmcCloseConnection(connection, 0, nullptr);
}
