// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "session_windows.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>

int main() {
  char temporary[] = "/tmp/midnight-window-channel-XXXXXX";
  if (!mkdtemp(temporary)) return 1;
  const std::filesystem::path root(temporary), path = root / "windows.bin";
  bool passed = true;
  const auto check = [&](bool value, const char* message) {
    if (!value) { passed = false; std::cerr << message << '\n'; }
  };
  const std::vector<midnight::ApplicationWindow> windows{
    {"client", std::string("role-\xff", 6), {-500,100,100,80}, 2, false,false,false,true}};
  check(bool(midnight::write_window_report(path, windows)), "report save failed");
  auto loaded = midnight::read_window_report(path);
  check(loaded && loaded->windows == windows, "binary report changed values");
  check(bool(midnight::write_window_report(path, std::vector<midnight::ApplicationWindow>{})), "empty report save failed");
  loaded = midnight::read_window_report(path);
  check(loaded && loaded->windows.empty(), "empty report failed");
  midnight::WindowCapture capture(windows);
  capture.desktop_entries = {"org.example.Editor.desktop"};
  check(bool(midnight::write_window_report(path, capture)), "desktop report save failed");
  loaded = midnight::read_window_report(path);
  check(loaded && loaded->windows == windows && loaded->desktop_entries == capture.desktop_entries, "desktop identities lost in native report");
  capture.desktop_entries.push_back(capture.desktop_entries.front());
  check(!midnight::write_window_report(path, capture), "duplicate desktop identity accepted");
  capture.desktop_entries = {"../bad.desktop"};
  check(!midnight::write_window_report(path, capture), "unsafe desktop identity accepted");
  auto duplicate = windows; duplicate.push_back(windows.front());
  check(!midnight::write_window_report(path, duplicate), "duplicate identity accepted");
  for (const auto& bad : {std::string("MWP2"), std::string("MWP1not-valid"), std::string("MWP2not-valid"), std::string(1024*1024+1,'x')}) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bad;
    check(!midnight::read_window_report(path), "malformed binary report accepted");
  }
  {
    midnight::SessionWindowChannel channel;
    check(bool(channel.initialize(root)), "socket channel failed");
    (void)channel.release_child_fd(); // Reporter consumes the advertised descriptor.
    midnight::SessionWindowReporter reporter;
    reporter.ready(); channel.tick();
    check(channel.attached(), "reporter did not attach");
    channel.request();
    check(channel.pending(), "capture request not pending");
    reporter.tick([&] { return midnight::WindowReport(windows); });
    channel.tick(); loaded = channel.result();
    check(!channel.pending() && loaded && loaded->windows == windows, "capture transaction failed");
    check(reporter.request_logout(), "logout request fell back to standalone");
    channel.tick();
    check(channel.take_logout_request() && !channel.take_logout_request(), "logout request not consumed once");
    channel.logout_reply(false);
    reporter.tick([&] { return midnight::WindowReport(windows); });
    check(reporter.logout_result() == false, "logout cancellation not delivered");
    check(!reporter.logout_result(), "logout result not consumed once");
    reporter.request_logout(); channel.tick();
    check(channel.take_logout_request(), "logout retry not delivered");
    channel.logout_reply(true);
    reporter.tick([&] { return midnight::WindowReport(windows); });
    check(reporter.logout_result() == true, "logout approval not delivered");
    channel.request();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    // A reply queued after the deadline must fail even when the parent has
    // not pumped its channel between the timeout and that late reply.
    reporter.tick([&] { return midnight::WindowReport(windows); });
    channel.tick();
    check(!channel.pending() && !channel.result(), "capture timeout not bounded");
    channel.request();
    check(!channel.pending() && !channel.result(), "late capture authorized later request");
    reporter.finish(windows); channel.tick(); channel.request();
    loaded = channel.result();
    check(loaded && loaded->windows == windows, "final shutdown capture not retained");
  }
  check(!std::filesystem::exists(path), "runtime window report leaked");
  std::filesystem::remove_all(root);
  return passed ? 0 : 1;
}
