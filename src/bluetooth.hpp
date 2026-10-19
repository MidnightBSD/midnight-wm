// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace midnight {
// hccontrol runs without a shell, asynchronously; all X11 work stays in Shell::tick.
class BluetoothMonitor {
public:
  explicit BluetoothMonitor(std::string tool = "/usr/sbin/hccontrol");
  ~BluetoothMonitor();
  BluetoothMonitor(const BluetoothMonitor&) = delete;
  BluetoothMonitor& operator=(const BluetoothMonitor&) = delete;
  void refresh(bool scan = false);
  bool tick(bool visible);
  bool busy() const;
  bool available() const { return available_; }
  const std::vector<std::string>& rows() const { return rows_; }
  static std::vector<std::string> adapters(const std::string& output);
  static std::vector<std::string> device_rows(const std::string& output, bool connected);
private:
  struct Job;
  struct Command { std::string node, action; };
  std::string tool_;
  std::shared_ptr<Job> job_;
  std::vector<Command> queue_;
  std::vector<std::string> rows_{"Open to refresh Bluetooth status"};
  std::chrono::steady_clock::time_point last_refresh_{};
  bool scan_{}, available_{};
  void start(Command command);
};
}
