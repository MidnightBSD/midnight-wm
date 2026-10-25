// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "chat_core.hpp"
#include <optional>
#include <string_view>

namespace midnight::chat {
struct ToolPaths {
  std::string locate{"/usr/bin/locate"};
  std::string installed{"/var/db/mport/master.db"};
  std::string available{"/var/db/mport/index.db"};
};
// Descriptions contain no credentials. Executors accept only the advertised schema.
std::string tool_definitions(const Settings& settings);
std::string execute_tool(const Settings& settings, const std::string& name,
    const std::string& arguments, GCancellable* cancel, const ToolPaths& paths = {});
std::string search_result_text(const std::string& response, bool brave);
void validate_tool_settings(const Settings& settings);
bool tools_enabled(const Settings& settings);
// Parsers for Linux /proc text used by system_information; exposed for tests.
// Values are bounded and sanitized; absent or malformed input yields no value.
std::string proc_cpu_model(const std::string& cpuinfo);
std::optional<unsigned long long> proc_memory_bytes(const std::string& meminfo);
std::optional<unsigned long long> proc_uptime_seconds(const std::string& uptime);
}
