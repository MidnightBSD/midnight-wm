// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "content_size.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

using namespace midnight;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

static void wait_for(const std::function<bool()>& condition) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!condition()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(5ms);
  }
}
static std::string uri(const fs::path& path) {
  auto* file = g_file_new_for_path(path.c_str());
  auto* value = g_file_get_uri(file);
  std::string result(value);
  g_free(value);
  g_object_unref(file);
  return result;
}

int main() {
  assert(format_content_size({0, true, false}) == "0 B");
  assert(format_content_size({1023, true, false}) == "1023 B");
  assert(format_content_size({1024, true, false}) == "1.0 KiB");
  assert(format_content_size({1536, true, false}) == "1.5 KiB");
  assert(format_content_size({1048576, true, false}) == "1.0 MiB");
  assert(format_content_size({1073741824, true, false}) == "1.0 GiB");
  assert(format_content_size({1099511627776, true, false}) == "1.0 TiB");
  assert(format_content_size({1024, true, true}) == "1.0 KiB (partial)");
  assert(format_content_size({}) == "Size unavailable");
  char pattern[] = "/tmp/midnight-content-size-XXXXXX";
  const auto* created = mkdtemp(pattern);
  assert(created);
  const fs::path root(created);
  fs::create_directories(root / "nested/empty");
  std::ofstream(root / "first").put('a');
  std::ofstream(root / "nested/second").put('b');
  fs::resize_file(root / "first", 1024);
  fs::resize_file(root / "nested/second", 1048576);
  fs::create_directory_symlink("..", root / "nested/loop");
  fs::create_symlink("missing", root / "dangling");
  // A FIFO must not be opened or read by the metadata-only traversal.
  assert(mkfifo((root / "pipe").c_str(), 0600) == 0);
  auto* cancel = g_cancellable_new();
  auto result = measure_content_size(uri(root), cancel);
  assert(result.available && !result.partial && result.bytes == 1048576 + 1024 + 2 + 7);
  assert(measure_content_size(uri(root / "first"), cancel).bytes == 1024);
  assert(measure_content_size(uri(root / "nested/empty"), cancel).bytes == 0);
  assert(!measure_content_size(uri(root / "missing"), cancel).available);
  g_cancellable_cancel(cancel);
  assert(!measure_content_size(uri(root), cancel).available);
  g_object_unref(cancel);

  ContentSize actual;
  actual.request(uri(root));
  wait_for([&] { actual.poll(); return actual.result().has_value(); });
  assert(actual.result()->bytes == result.bytes);
  fs::resize_file(root / "first", 2048);
  actual.request(uri(root), true);
  assert(!actual.result());
  wait_for([&] { actual.poll(); return actual.result().has_value(); });
  assert(actual.result()->bytes == result.bytes + 1024);

  struct Probe { std::atomic<int> started{}, finished{}, cancelled{}; };
  auto probe = std::make_shared<Probe>();
  ContentSize view([probe](const std::string& target, GCancellable* cancel) {
    ++probe->started;
    if (target == "slow") {
      while (!g_cancellable_is_cancelled(cancel)) std::this_thread::sleep_for(1ms);
      ++probe->cancelled;
    }
    ++probe->finished;
    return ContentSizeResult{target == "slow" ? 999 : 42, true, false};
  });
  view.request("slow");
  wait_for([&] { view.poll(); return probe->started.load() == 1; });
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 100; ++i) view.request("discard-" + std::to_string(i));
  view.request("latest");
  assert(std::chrono::steady_clock::now() - start < 100ms);
  wait_for([&] { view.poll(); return view.result().has_value(); });
  assert(view.result()->bytes == 42); // cancelled/stale 999 must never be displayed
  assert(probe->started == 2 && probe->cancelled == 1);

  auto closed = std::make_shared<Probe>();
  {
    ContentSize closing([closed](const std::string&, GCancellable* cancel) {
      ++closed->started;
      while (!g_cancellable_is_cancelled(cancel)) std::this_thread::sleep_for(1ms);
      ++closed->cancelled;
      ++closed->finished;
      return ContentSizeResult{};
    });
    closing.request("closing");
    wait_for([&] { closing.poll(); return closed->started.load() == 1; });
  }
  wait_for([&] { return closed->finished.load() == 1; });
  assert(closed->cancelled == 1);

  // Pathological depth is reported as a partial total, never a complete zero.
  auto deep = root / "deep";
  fs::create_directory(deep);
  for (int i = 0; i < 130; ++i) { deep /= "d"; fs::create_directory(deep); }
  cancel = g_cancellable_new();
  assert(measure_content_size(uri(root / "deep"), cancel).partial);
  g_object_unref(cancel);
  fs::remove_all(root);
}
