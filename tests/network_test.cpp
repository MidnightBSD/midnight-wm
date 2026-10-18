// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "network.hpp"
#include <iostream>
using namespace midnight;
int main() {
  using namespace std::chrono_literals;
  int reads = 0, details = 0;
  bool fail = false;
  std::vector<NetworkInterface> interfaces;
  NetworkMonitor monitor([&](bool detail) -> std::expected<std::vector<NetworkInterface>, std::string> {
    ++reads; if (detail) ++details;
    if (fail) return std::unexpected("unavailable");
    return interfaces;
  });
  auto now = NetworkMonitor::Clock::time_point{};
  const auto check = [](bool value, const char* message) { if (!value) std::cerr << message << '\n'; return value; };
  monitor.poll(false, false, now);
  if (!check(reads == 1 && details == 0 && monitor.state().kind == NetworkKind::disconnected, "initial disconnected state")) return 1;
  interfaces.push_back({.name="wlan0", .kind=NetworkKind::wifi, .connected=true});
  monitor.poll(false, false, now + 29s);
  if (!check(reads == 1, "closed popup polled before 30 seconds")) return 1;
  monitor.poll(false, false, now + 30s);
  if (!check(reads == 2 && details == 0 && monitor.state().kind == NetworkKind::wifi, "Wi-Fi status not refreshed")) return 1;
  const auto since = monitor.state().interfaces[0].connected_since;
  monitor.poll(true, true, now + 31s);
  monitor.poll(true, false, now + 32s);
  if (!check(reads == 3 && details == 1, "details must be sampled only once on open")) return 1;
  monitor.poll(true, false, now + 33s);
  if (!check(reads == 4 && details == 2 && monitor.state().interfaces[0].connected_since == since, "detail cadence or connection continuity")) return 1;
  monitor.poll(false, false, now + 62s);
  if (!check(reads == 4, "closing popup continued detail sampling")) return 1;
  interfaces.push_back({.name="em0", .kind=NetworkKind::wired, .connected=true});
  interfaces.push_back({.name="wlan1", .kind=NetworkKind::wifi, .connected=false});
  monitor.poll(false, false, now + 63s);
  if (!check(reads == 5 && details == 2 && monitor.state().kind == NetworkKind::wired && monitor.state().interfaces.size() == 2, "multiple active interfaces")) return 1;
  fail = true;
  monitor.poll(false, true, now + 64s);
  if (!check(monitor.state().kind == NetworkKind::unknown && !monitor.state().error.empty(), "read failure misreported as disconnected")) return 1;
  fail = false; interfaces.clear();
  monitor.poll(false, true, now + 65s);
  interfaces.push_back({.name="wlan0", .kind=NetworkKind::wifi, .connected=true});
  monitor.poll(true, true, now + 66s);
  if (!check(monitor.state().interfaces[0].connected_since == now + 66s, "reconnection did not reset observed time")) return 1;
  interfaces = {{.name="lagg0", .kind=NetworkKind::wifi, .connected=true},
                {.name="wlan0", .kind=NetworkKind::wifi, .connected=true, .parent="lagg0"},
                {.name="em0", .kind=NetworkKind::wired, .connected=true, .parent="lagg0"}};
  monitor.poll(false, true, now + 67s);
  if (!check(monitor.state().kind == NetworkKind::wifi, "standby wired lagg member hid active Wi-Fi")) return 1;
  NetworkMonitor native;
  native.poll(true, true);
  std::cout << "Native kind: " << static_cast<int>(native.state().kind) << '\n';
  std::cout << "Native interfaces: " << native.state().interfaces.size() << '\n';
  for (const auto& item : native.state().interfaces) {
    std::cout << item.name << " SSID=" << item.ssid << " standard=" << item.standard
              << " speed=" << item.speed_mbps.value_or(0) << " RX=" << item.received.value_or(0)
              << " TX=" << item.transmitted.value_or(0) << '\n';
    for (const auto& address : item.addresses) std::cout << "IP=" << address << '\n';
  }
  return 0;
}
