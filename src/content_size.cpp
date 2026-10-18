// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "content_size.hpp"

#include <sys/types.h>
#include <libutil.h>
#include <atomic>
#include <limits>
#include <utility>
#include <vector>

namespace midnight {
namespace {
struct Unref {
  template<class T> void operator()(T* object) const { if (object) g_object_unref(object); }
};
template<class T> using Object = std::unique_ptr<T, Unref>;
constexpr auto attributes = "standard::name,standard::type,standard::size";
constexpr auto nofollow = G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS;
void add_size(ContentSizeResult& result, GFileInfo* info) {
  const auto type = g_file_info_get_file_type(info);
  if (type != G_FILE_TYPE_REGULAR && type != G_FILE_TYPE_SYMBOLIC_LINK) return;
  if (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
    result.partial = true;
    return;
  }
  const auto size = g_file_info_get_size(info);
  if (size < 0 || size > std::numeric_limits<std::int64_t>::max() - result.bytes) {
    result.partial = true;
    return;
  }
  result.bytes += size;
}
}

ContentSizeResult measure_content_size(const std::string& uri, GCancellable* cancel) {
  ContentSizeResult result;
  Object<GFile> root(g_file_new_for_uri(uri.c_str()));
  Object<GFileInfo> info(g_file_query_info(root.get(), attributes, nofollow, cancel, nullptr));
  if (!info) return result;
  result.available = true;
  if (g_file_info_get_file_type(info.get()) != G_FILE_TYPE_DIRECTORY) {
    add_size(result, info.get());
    return result;
  }
  struct Directory {
    Object<GFile> file;
    Object<GFileEnumerator> entries;
  };
  std::vector<Directory> stack;
  auto descend = [&](Object<GFile> file) {
    Object<GFileEnumerator> entries(g_file_enumerate_children(file.get(), attributes, nofollow, cancel, nullptr));
    if (!entries) { result.partial = true; return; }
    stack.push_back({std::move(file), std::move(entries)});
  };
  descend(std::move(root));
  while (!stack.empty() && !g_cancellable_is_cancelled(cancel)) {
    auto& directory = stack.back();
    GError* error = nullptr;
    Object<GFileInfo> child(g_file_enumerator_next_file(directory.entries.get(), cancel, &error));
    if (!child) {
      if (error) { result.partial = true; g_error_free(error); }
      stack.pop_back();
      continue;
    }
    if (g_file_info_get_file_type(child.get()) == G_FILE_TYPE_DIRECTORY) {
      // Bound open enumerators and protect against pathological directory depth.
      if (stack.size() >= 128) { result.partial = true; continue; }
      Object<GFile> file(g_file_get_child(directory.file.get(), g_file_info_get_name(child.get())));
      descend(std::move(file));
    } else add_size(result, child.get());
  }
  if (g_cancellable_is_cancelled(cancel)) result.partial = true;
  return result;
}

std::string format_content_size(const ContentSizeResult& result) {
  if (!result.available) return "Size unavailable";
  // HN_AUTOSCALE chooses a unit based on buffer width. Pick the binary scale
  // explicitly so 1024 bytes is always KiB, even with a generous output buffer.
  int scale = 0;
  for (auto value = result.bytes; value >= 1024 && scale < 6; value /= 1024) ++scale;
  char buffer[32];
  if (humanize_number(buffer, sizeof(buffer), result.bytes, "B", scale,
                      HN_DECIMAL | HN_IEC_PREFIXES) < 0) return "Size unavailable";
  return std::string(buffer) + (result.partial ? " (partial)" : "");
}

struct ContentSize::Work {
  Object<GCancellable> cancel{g_cancellable_new()};
  Reader reader;
  std::string uri;
  ContentSizeResult result;
  std::atomic<bool> done{};
};

ContentSize::ContentSize(Reader reader) : reader_(std::move(reader)) {}
ContentSize::~ContentSize() {
  if (work_) g_cancellable_cancel(work_->cancel.get());
}

void ContentSize::request(std::string uri, bool invalidate) {
  if (!invalidate && uri == uri_) return;
  uri_ = std::move(uri);
  result_.reset();
  dirty_ = true;
  deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
  if (work_) g_cancellable_cancel(work_->cancel.get());
}

bool ContentSize::poll() {
  const auto now = std::chrono::steady_clock::now();
  bool changed = false;
  if (work_ && work_->done.load(std::memory_order_acquire)) {
    if (!dirty_) {
      result_ = work_->result;
      completed_ = now;
      changed = true;
    }
    work_.reset();
  }
  // Top-level directory watches do not see every change inside descendants.
  // Resample idle views periodically without interrupting a large traversal.
  if (!work_ && !dirty_ && result_ && now - completed_ >= std::chrono::seconds(30)) {
    request(uri_, true);
    changed = true;
  }
  if (!work_ && dirty_ && now >= deadline_) {
    dirty_ = false;
    work_ = std::make_shared<Work>();
    work_->uri = uri_;
    work_->reader = reader_;
    auto* task = g_task_new(nullptr, work_->cancel.get(), nullptr, nullptr);
    g_task_set_task_data(task, new std::shared_ptr<Work>(work_),
                        +[](gpointer data) { delete static_cast<std::shared_ptr<Work>*>(data); });
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer data, GCancellable*) {
      const auto work = *static_cast<std::shared_ptr<Work>*>(data);
      try {
        if (!g_cancellable_is_cancelled(work->cancel.get()))
          work->result = work->reader(work->uri, work->cancel.get());
      } catch (...) {
        work->result = {};
      }
      work->done.store(true, std::memory_order_release);
      g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
  }
  return changed;
}
} // namespace midnight
