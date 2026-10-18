// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include <X11/SM/SMlib.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <poll.h>
#include <unistd.h>

namespace {
struct Client {
  SmcConn connection{};
  std::string mode;
  int saves{}, completes{}, queries{};
  bool valid{true}, expect_command{true};
};
void properties(SmcConn connection) {
  char program[] = "midnight-xsmp-fixture";
  char executable[] = "/usr/local/bin/midnight-xsmp-fixture";
  char option[] = "--restore";
  char argument[] = "literal $(not-a-command) with spaces";
  std::array<SmPropValue, 1> program_values{{{int(std::strlen(program)), program}}};
  std::array<SmPropValue, 3> arguments{{{int(std::strlen(executable)), executable},
    {int(std::strlen(option)), option}, {int(std::strlen(argument)), argument}}};
  char opaque_bytes[]{'a', '\0', 'b'};
  std::array<SmPropValue, 1> opaque{{{3, opaque_bytes}}};
  SmProp name{const_cast<char*>(SmProgram), const_cast<char*>(SmARRAY8), 1, program_values.data()};
  SmProp command{const_cast<char*>(SmRestartCommand), const_cast<char*>(SmLISTofARRAY8), 3, arguments.data()};
  SmProp custom{const_cast<char*>("MidnightTestOpaque"), const_cast<char*>(SmARRAY8), 1, opaque.data()};
  std::array<SmProp*, 3> values{&name, &command, &custom};
  SmcSetProperties(connection, int(values.size()), values.data());
}
void phase2(SmcConn connection, SmPointer) {
  properties(connection);
  SmcSaveYourselfDone(connection, True);
}
void save(SmcConn connection, SmPointer data, int, Bool shutdown, int interact, Bool) {
  auto& client = *static_cast<Client*>(data);
  ++client.saves;
  client.valid = client.valid && !shutdown && interact == SmInteractStyleNone;
  if (client.mode == "hang") return;
  if (client.mode == "phase2") {
    client.valid = client.valid && SmcRequestSaveYourselfPhase2(connection, phase2, data);
    return;
  }
  properties(connection);
  SmcSaveYourselfDone(connection, True);
}
void complete(SmcConn, SmPointer data) { ++static_cast<Client*>(data)->completes; }
void die(SmcConn, SmPointer data) { static_cast<Client*>(data)->valid = false; }
void cancelled(SmcConn, SmPointer data) { static_cast<Client*>(data)->valid = false; }
void query(SmcConn, SmPointer data, int count, SmProp** props) {
  auto& client = *static_cast<Client*>(data);
  bool command{}, opaque{}, program{};
  for (int i = 0; i < count; ++i) {
    const auto* p = props[i];
    if (std::strcmp(p->name, SmRestartCommand) == 0) {
      command = std::strcmp(p->type, SmLISTofARRAY8) == 0 && p->num_vals == 3 &&
        std::string(static_cast<char*>(p->vals[2].value), p->vals[2].length) == "literal $(not-a-command) with spaces";
    } else if (std::strcmp(p->name, SmProgram) == 0) {
      program = p->num_vals == 1 && std::string(static_cast<char*>(p->vals[0].value), p->vals[0].length) == "midnight-xsmp-fixture";
    } else if (std::strcmp(p->name, "MidnightTestOpaque") == 0) {
      opaque = p->num_vals == 1 && p->vals[0].length == 3 &&
        std::memcmp(p->vals[0].value, "a\0b", 3) == 0;
    }
    SmFreeProperty(props[i]);
  }
  std::free(props);
  client.valid = client.valid && command == client.expect_command && opaque && program;
  ++client.queries;
}
bool wait(Client& client, const auto& ready) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!ready() && client.valid && std::chrono::steady_clock::now() < deadline) {
    pollfd fd{IceConnectionNumber(SmcGetIceConnection(client.connection)), POLLIN, 0};
    if (poll(&fd, 1, 100) > 0) {
      if (IceProcessMessages(SmcGetIceConnection(client.connection), nullptr, nullptr) != IceProcessMessagesSuccess)
        return false;
    }
  }
  return ready() && client.valid;
}
}

int main(int argc, char** argv) {
  Client client;
  client.mode = argc > 1 ? argv[1] : "normal";
  SmcCallbacks callbacks{};
  callbacks.save_yourself = {save, &client};
  callbacks.die = {die, &client};
  callbacks.save_complete = {complete, &client};
  callbacks.shutdown_cancelled = {cancelled, &client};
  char error[256]{};
  char* id{};
  client.connection = SmcOpenConnection(nullptr, nullptr, SmProtoMajor, SmProtoMinor,
    SmcSaveYourselfProcMask | SmcDieProcMask | SmcSaveCompleteProcMask | SmcShutdownCancelledProcMask,
    &callbacks, client.mode == "reject" ? nullptr : "unknown-previous-id", &id, sizeof(error), error);
  if (client.mode == "reject") {
    if (client.connection) { SmcCloseConnection(client.connection, 0, nullptr); std::free(id); return 1; }
    return 0;
  }
  if (!client.connection) { std::cerr << "SM connection failed: " << error << '\n'; return 1; }
  const bool registered = id && *id;
  std::free(id);
  bool passed = registered && wait(client, [&] { return client.completes >= 1; });
  if (client.mode == "hang") {
    SmcCloseConnection(client.connection, 0, nullptr);
    return passed ? 0 : 1;
  }
  if (client.mode == "abrupt") _exit(passed ? 0 : 1);
  if (passed) {
    passed = SmcGetProperties(client.connection, query, &client) && wait(client, [&] { return client.queries >= 1; });
    char* name = const_cast<char*>(SmRestartCommand);
    SmcDeleteProperties(client.connection, 1, &name);
    client.expect_command = false;
    passed = passed && SmcGetProperties(client.connection, query, &client) && wait(client, [&] { return client.queries >= 2; });
    properties(client.connection);
    client.expect_command = true;
    // A malformed update must leave the previously accepted command intact.
    char invalid[] = "wrong type";
    SmPropValue value{int(std::strlen(invalid)), invalid};
    SmProp bad{const_cast<char*>(SmRestartCommand), const_cast<char*>(SmARRAY8), 1, &value};
    SmProp* pointer = &bad;
    SmcSetProperties(client.connection, 1, &pointer);
    passed = passed && SmcGetProperties(client.connection, query, &client) && wait(client, [&] { return client.queries >= 3; });
    for (bool global : {false, true}) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1100));
      const auto before = client.completes;
      if (global && client.mode == "signal") {
        const long manager = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 0;
        passed = passed && manager > 1 && kill(static_cast<pid_t>(manager), SIGUSR1) == 0;
      } else SmcRequestSaveYourself(client.connection, SmSaveBoth, False, SmInteractStyleNone, True, global);
      passed = passed && wait(client, [&] { return client.completes > before; });
    }
  }
  SmcCloseConnection(client.connection, 0, nullptr);
  if (passed && client.valid) std::cout << "XSMP registration, checkpoint, properties, deletion and " << client.mode << " passed\n";
  return passed && client.valid ? 0 : 1;
}
