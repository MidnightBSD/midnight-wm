// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "bluetooth.hpp"
#include <glib.h>
#include <algorithm>
#include <iostream>
#include <thread>
using namespace midnight;
int main(int argc, char** argv) {
  if (argc != 2) return 1;
  if (BluetoothMonitor::adapters("Name ID Hooks\nubt0hci 1 2\n../evilhci 2 3\nubt0hci 1 2\n").size() != 1) return 2;
  if (BluetoothMonitor::device_rows("Remote BD_ADDR\n01:02:03:04:05:06 42 ACL 0 MAST NONE 0 0 OPEN\n", true) !=
      std::vector<std::string>{"01:02:03:04:05:06 — Connected link"}) return 3;
  if (!BluetoothMonitor::device_rows("B ::::: 00\n", false).empty()) return 8;
  if (BluetoothMonitor::device_rows("01:02:03:04:05:06 0 CLOSED\n", true).at(0).find("pending/closed") == std::string::npos) return 9;
  auto wait = [](BluetoothMonitor& monitor) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (monitor.busy() && std::chrono::steady_clock::now() < limit) {
      while (g_main_context_iteration(nullptr, false)) {}
      monitor.tick(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return !monitor.busy();
  };
  BluetoothMonitor monitor(argv[1]);
  monitor.refresh(true);
  monitor.refresh(); // overlapping refresh must not replace the scan
  if (!wait(monitor) || !monitor.available()) return 4;
  int connected = 0, discovered = 0, scanned = 0, denied = 0;
  for (const auto& row : monitor.rows()) {
    connected += row.find("Connected link") != std::string::npos;
    discovered += row.find("Discovered") != std::string::npos;
    scanned += row.find("Found in scan") != std::string::npos;
    denied += row.find("Permission denied") != std::string::npos;
  }
  if (connected != 2 || discovered != 2 || scanned != 1 || denied != 1) return 5;
  monitor.refresh();
  if (!wait(monitor) || std::ranges::any_of(monitor.rows(), [](const auto& r) { return r.find("Permission denied") != std::string::npos; })) return 6;
  BluetoothMonitor absent("/nonexistent/midnight-bluetooth-tool");
  absent.refresh();
  if (!wait(absent) || absent.available()) return 7;
  // Shutdown while a callback owns a pending job must not access the destroyed monitor.
  { BluetoothMonitor closing(argv[1]); closing.refresh(); }
  for (int i = 0; i < 50; ++i) {
    while (g_main_context_iteration(nullptr, false)) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::cout << "Bluetooth parsing, multi-adapter queries, scan errors, refresh and shutdown passed\n";
}
