// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <gio/gio.h>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace midnight::chat {
inline constexpr std::size_t max_thread_messages = 100000;
// Saved-history field limits. load_threads rejects larger fields and
// save_threads clamps to them, so a file this program writes always reloads.
inline constexpr std::size_t max_title_bytes = 256, max_summary_bytes = 32768,
    max_message_bytes = 256 * 1024, max_tool_arguments_bytes = 4096, max_tool_result_bytes = 8192;

struct Settings {
  std::string url{"http://localhost/v1"}, model, token;
  int port{11434}, context{8192}, reply{1024};
  int tool_rounds{12}, tool_calls{24}, request_timeout{900};
  bool authenticate{};
  bool file_search{}, system_information{}, package_queries{}, web_search{};
  std::string search_provider{"brave"}, search_url, search_token;
};
inline constexpr std::size_t max_servers = 32;
struct ServerProfile {
  std::string id, name;
  Settings settings;
};
struct ServerProfiles {
  std::string selected;
  std::vector<ServerProfile> servers;
};
ServerProfile new_server_profile(const std::string& name);
ServerProfiles load_server_profiles(const std::filesystem::path& path);
void save_server_profiles(const std::filesystem::path& path, const ServerProfiles& profiles);
struct ToolUse { std::string id, name, arguments, result; };
struct Message {
  std::string role, text;
  std::vector<ToolUse> tools{};
};
struct Thread {
  std::string id, title{"New conversation"}, summary;
  std::size_t compacted{};
  std::string summary_model;
  int summary_context{};
  std::size_t last_context_cost{};
  std::vector<Message> messages;
};
// Throw std::runtime_error on invalid input, storage, protocol or transport errors.
std::string endpoint(const Settings& settings);
// True when an enabled bearer token would travel over plain HTTP to a host
// other than loopback. False for invalid URLs; validate() reports those.
bool cleartext_credentials(const Settings& settings);
std::vector<std::string> models(const Settings& settings, GCancellable* cancel);
Settings load_settings(const std::filesystem::path& path);
void save_settings(const std::filesystem::path& path, const Settings& settings);
std::vector<Thread> load_threads(const std::filesystem::path& path);
void save_threads(const std::filesystem::path& path, const std::vector<Thread>& threads);
struct ContextInfo {
  int tokens{};
  std::string source;
};

// Compaction progress callback: reports how many messages have been compacted
using CompactionProgress = std::function<void(std::size_t compacted, std::size_t total)>;

// Cached context limit information for performance
struct CachedContextInfo {
  std::string model;
  std::string url;
  int port{};
  ContextInfo info;
  std::chrono::system_clock::time_point timestamp;
};

// Best-effort, bounded discovery. The configured context remains an upper bound.
ContextInfo context_limit(const Settings& settings, GCancellable* cancel);

// Token estimation helper - better than 1 byte = 1 token
std::size_t estimate_tokens(const Settings& s, const std::string& text);

// Context budget calculation with tool awareness
std::size_t context_budget(const Settings& s, const Thread& t = {});

// Improved summarization with adaptive chunking and progress
void summarize(const Settings& s, Thread& t, std::size_t target, GCancellable* cancel, bool force,
    const CompactionProgress& progress = {});

// Thread compaction with metadata tracking
Thread compact(const Settings& settings, Thread thread, GCancellable* cancel, ContextInfo* info = nullptr);
// Runs off the GTK thread. Updates a copy, committing summaries only on success.
// Asked from the worker thread before web_search runs after a local tool in the
// same reply, because the query may carry local results to the search provider.
// Return false to decline that search. Without a callback, searches proceed.
using WebSearchConfirm = std::function<bool(const std::string& query)>;
Thread complete(const Settings& settings, Thread thread, GCancellable* cancel, ContextInfo* info = nullptr,
    const WebSearchConfirm& confirm = {});

// Global context cache for performance optimization
extern std::unordered_map<std::string, CachedContextInfo> context_cache;

}
