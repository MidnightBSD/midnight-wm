// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <gio/gio.h>
#include <filesystem>
#include <string>
#include <vector>

namespace midnight::chat {
struct Settings {
  std::string url{"http://localhost/v1"}, model, token;
  int port{11434}, context{8192}, reply{1024};
  bool authenticate{};
};
struct Message { std::string role, text; };
struct Thread {
  std::string id, title{"New conversation"}, summary;
  std::size_t compacted{};
  std::vector<Message> messages;
};
// Throw std::runtime_error on invalid input, storage, protocol or transport errors.
std::string endpoint(const Settings& settings);
std::vector<std::string> models(const Settings& settings, GCancellable* cancel);
Settings load_settings(const std::filesystem::path& path);
void save_settings(const std::filesystem::path& path, const Settings& settings);
std::vector<Thread> load_threads(const std::filesystem::path& path);
void save_threads(const std::filesystem::path& path, const std::vector<Thread>& threads);
// Runs off the GTK thread. Updates a copy, committing summaries only on success.
Thread complete(const Settings& settings, Thread thread, GCancellable* cancel);
}
