// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace midnight {
enum class NetworkKind { disconnected, wired, wifi, unknown };
struct NetworkInterface {
  std::string name;
  NetworkKind kind{NetworkKind::unknown};
  bool connected{};
  std::string parent;
  std::vector<std::string> addresses;
  std::optional<std::uint64_t> received, transmitted;
  std::string ssid, standard;
  std::optional<double> speed_mbps;
  std::chrono::steady_clock::time_point connected_since{};
  bool operator==(const NetworkInterface&) const = default;
};
struct NetworkState {
  NetworkKind kind{NetworkKind::disconnected};
  std::vector<NetworkInterface> interfaces;
  std::string error;
  bool operator==(const NetworkState&) const = default;
};

// No X11 dependency. Expensive Wi-Fi queries are made only for open details.
class NetworkMonitor {
public:
  using Clock = std::chrono::steady_clock;
  using Reader = std::function<std::expected<std::vector<NetworkInterface>, std::string>(bool details)>;
  NetworkMonitor();
  explicit NetworkMonitor(Reader reader);
  bool poll(bool details, bool force = false, Clock::time_point now = Clock::now());
  const NetworkState& state() const { return state_; }
private:
  Reader reader_;
  NetworkState state_;
  std::optional<Clock::time_point> last_status_, last_details_;
  std::map<std::string, Clock::time_point> connected_since_;
};
} // namespace midnight
