// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <gio/gio.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace midnight {
struct ContentSizeResult {
  std::int64_t bytes{};
  bool available{};
  bool partial{};
};

// Metadata-only traversal. Directory contents are recursive; symlinks are
// measured as links and never followed. Special files contribute no bytes.
[[nodiscard]] ContentSizeResult measure_content_size(const std::string& uri, GCancellable* cancel);
[[nodiscard]] std::string format_content_size(const ContentSizeResult& result);

// Owned by a view, used only on the main thread. Workers own their input and
// result state, never the view. At most one traversal per view is in flight;
// replacements are cancelled and coalesced until that traversal finishes.
class ContentSize {
public:
  using Reader = std::function<ContentSizeResult(const std::string&, GCancellable*)>;
  explicit ContentSize(Reader reader = measure_content_size);
  ~ContentSize();
  ContentSize(const ContentSize&) = delete;
  ContentSize& operator=(const ContentSize&) = delete;
  void request(std::string uri, bool invalidate = false);
  // Poll from the owning tick; true means the displayed result changed.
  bool poll();
  [[nodiscard]] const std::optional<ContentSizeResult>& result() const { return result_; }
private:
  struct Work;
  Reader reader_;
  std::string uri_;
  std::shared_ptr<Work> work_;
  std::optional<ContentSizeResult> result_;
  bool dirty_{};
  std::chrono::steady_clock::time_point deadline_{}, completed_{};
};
} // namespace midnight
