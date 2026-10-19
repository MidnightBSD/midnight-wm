// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "bluetooth.hpp"
#include <gio/gio.h>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace midnight {
struct BluetoothMonitor::Job {
  GSubprocess* process{};
  Command command;
  std::string output, error;
  bool done{}, timed_out{};
  std::chrono::steady_clock::time_point deadline;
  ~Job() { if (process) g_object_unref(process); }
};
BluetoothMonitor::BluetoothMonitor(std::string tool) : tool_(std::move(tool)) {}
BluetoothMonitor::~BluetoothMonitor() {
  if (job_ && job_->process && !job_->done) g_subprocess_force_exit(job_->process);
}
bool BluetoothMonitor::busy() const { return job_ || !queue_.empty(); }
std::vector<std::string> BluetoothMonitor::adapters(const std::string& output) {
  std::vector<std::string> result;
  std::istringstream lines(output);
  for (std::string line; std::getline(lines, line) && result.size() < 16;) {
    std::istringstream fields(line);
    std::string node, id, hooks;
    if (!(fields >> node >> id >> hooks) || !node.ends_with("hci") || node.size() > 31) continue;
    if (!std::ranges::all_of(node, [](unsigned char c) { return std::isalnum(c) || c == '_'; })) continue;
    if (std::ranges::find(result, node) == result.end()) result.push_back(node);
  }
  return result;
}
std::vector<std::string> BluetoothMonitor::device_rows(const std::string& output, bool connected) {
  std::vector<std::string> result;
  std::istringstream lines(output);
  for (std::string line; std::getline(lines, line) && result.size() < 256;) {
    std::istringstream fields(line);
    std::string token;
    // Neighbor cache starts with an address-type column; connection lists do not.
    while (fields >> token) {
      if (token.size() > 17 || std::ranges::count(token, ':') != 5 ||
          !std::ranges::all_of(token, [](unsigned char c) { return std::isxdigit(c) || c == ':'; })) continue;
      std::istringstream octets(token);
      bool valid = true;
      for (std::string octet; std::getline(octets, octet, ':');)
        if (octet.empty() || octet.size() > 2) valid = false;
      if (token.back() == ':' || !valid) continue;
      std::string label = " — Discovered (cached)";
      if (connected) {
        std::string state, field;
        while (fields >> field) state = field;
        label = state == "OPEN" ? " — Connected link" : " — Link pending/closed";
      }
      result.push_back(token + label);
      break;
    }
  }
  return result;
}
void BluetoothMonitor::start(Command command) {
  job_ = std::make_shared<Job>();
  auto& job = *job_;
  job.command = std::move(command);
  job.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
  const char* argv[]{tool_.c_str(), "-N", nullptr, nullptr, nullptr, nullptr};
  if (job.command.node.empty()) argv[2] = job.command.action.c_str();
  else { argv[2] = "-n"; argv[3] = job.command.node.c_str(); argv[4] = job.command.action.c_str(); }
  GError* error = nullptr;
  GSubprocessLauncher* launcher = g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
  g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", true);
  job.process = g_subprocess_launcher_spawnv(launcher, argv, &error);
  g_object_unref(launcher);
  if (!job.process) {
    job.error = "Bluetooth tool unavailable";
    if (error) g_error_free(error);
    job.done = true;
    return;
  }
  auto* owned = new std::shared_ptr<Job>(job_);
  g_subprocess_communicate_utf8_async(job.process, nullptr, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<std::shared_ptr<Job>> holder(static_cast<std::shared_ptr<Job>*>(data));
        auto& finished = **holder;
        gchar *out = nullptr, *err = nullptr;
        GError* failure = nullptr;
        const bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err, &failure);
        if (out) finished.output = out;
        if (!ok || !g_subprocess_get_successful(finished.process)) {
          // Diagnostics are bounded and stripped of control characters before display.
          finished.error = err && *err ? std::string(err).substr(0, 180) : "Bluetooth query failed";
          for (char& c : finished.error) if (static_cast<unsigned char>(c) < 32) c = ' ';
        }
        g_free(out); g_free(err);
        if (failure) g_error_free(failure);
        finished.done = true;
      }, owned);
}
void BluetoothMonitor::refresh(bool scan) {
  if (busy()) return;
  scan_ = scan;
  rows_ = {scan ? "Scanning for Bluetooth devices…" : "Reading Bluetooth status…"};
  last_refresh_ = std::chrono::steady_clock::now();
  start({{}, "read_node_list"});
}
bool BluetoothMonitor::tick(bool visible) {
  const auto now = std::chrono::steady_clock::now();
  if (job_ && !job_->done && now >= job_->deadline && !job_->timed_out) {
    job_->timed_out = true;
    g_subprocess_force_exit(job_->process);
  }
  if (!job_) {
    if (visible && now - last_refresh_ >= std::chrono::seconds(30)) { refresh(); return true; }
    return false;
  }
  if (!job_->done) return false;
  auto finished = std::move(job_);
  if (finished->command.node.empty()) {
    rows_.clear();
    const auto nodes = adapters(finished->output);
    available_ = !nodes.empty();
    if (nodes.empty()) rows_.push_back("No usable Bluetooth controller found");
    for (const auto& node : nodes) {
      if (scan_) queue_.push_back({node, "inquiry"});
      queue_.push_back({node, "read_connection_list"});
      queue_.push_back({node, "read_neighbor_cache"});
    }
  } else {
    const auto& cmd = finished->command;
    if (cmd.action == "inquiry") {
      rows_.push_back(cmd.node + " — Scan results");
      auto devices = device_rows(finished->output, false);
      for (auto& device : devices) {
        const auto suffix = device.find(" — Discovered (cached)");
        if (suffix != std::string::npos) device.replace(suffix, std::string::npos, " — Found in scan");
      }
      if (devices.empty() && finished->error.empty()) rows_.push_back("No devices found");
      rows_.insert(rows_.end(), devices.begin(), devices.end());
    } else {
      rows_.push_back(cmd.node + (cmd.action == "read_connection_list" ? " — Connections" : " — Nearby devices"));
      auto devices = device_rows(finished->output, cmd.action == "read_connection_list");
      if (devices.empty() && finished->error.empty()) rows_.push_back("No devices reported");
      rows_.insert(rows_.end(), devices.begin(), devices.end());
    }
  }
  if (finished->timed_out) rows_.push_back("Bluetooth query timed out");
  else if (!finished->error.empty()) rows_.push_back(finished->error);
  if (!queue_.empty()) {
    auto next = std::move(queue_.front());
    queue_.erase(queue_.begin());
    start(std::move(next));
  } else {
    rows_.push_back("Pairing/setup: bluetooth-config scan (root terminal)");
    rows_.push_back("Cached devices may no longer be nearby");
  }
  return true;
}
}
