// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "chat_core.hpp"
#include "chat_tools.hpp"
#include <sqlite3.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
using namespace midnight::chat;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
  bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; }
  require(rejected, "Expected tool rejection");
}
int main(int argc, char** argv) {
  try {
    require(argc >= 4, "usage: chat_tools_test server-url port temp-dir [live-model]");
    Settings s; s.url = argv[1]; s.port = std::stoi(argv[2]); s.context = 16384; s.reply = 256;
    s.model = argc > 4 ? argv[4] : "fixture-tools";
    s.system_information = s.package_queries = true;
    auto* cancel = g_cancellable_new();
    Thread t; t.id = "b1f4a1c8-874f-42fb-ae1c-4eea2e33c81b";
    if (argc > 4) {
      t.messages.push_back({"user", "Use system_information to tell me the desktop OS and CPU. Also use mport_search with source installed and query curl to check installed curl packages. Do not guess; use both tools."});
      if (argc > 5) t.messages.back().text = "How many packages do I have installed on this system?";
      t = complete(s, t, cancel);
      bool system = false, packages = false, counted = false;
      for (const auto& tool : t.messages.back().tools) {
        std::cout << tool.name << " " << tool.arguments << "\n" << tool.result << '\n';
        system |= tool.name == "system_information";
        packages |= tool.name == "mport_search";
        counted |= tool.name == "mport_count";
      }
      std::cout << "Reply: " << t.messages.back().text << std::endl;
      require(argc > 5 ? counted : system && packages, "Live model did not use requested tools");
      save_threads(std::filesystem::path(argv[3]) / "history/threads.json", {t});
      g_object_unref(cancel); return 0;
    }
    const std::filesystem::path root(argv[3]);
    require(proc_cpu_model("processor\t: 0\nvendor_id\t: Fixture\nmodel name\t: Fixture CPU @ 2.00GHz  \n") == "Fixture CPU @ 2.00GHz", "cpuinfo model");
    require(proc_cpu_model("Model\t\t: Raspberry Pi 4 Model B\r\n") == "Raspberry Pi 4 Model B", "cpuinfo arm model");
    require(proc_cpu_model("processor: 0\nflags: fpu\nmodel name\nmodel name:\n").empty(), "cpuinfo without model");
    require(proc_cpu_model("model name: " + std::string(400, 'c') + "\n").size() <= 258, "cpuinfo bound");
    require(proc_memory_bytes("MemTotal:       16303052 kB\nMemFree:  1 kB\n") == 16303052ull * 1024, "meminfo");
    require(!proc_memory_bytes("MemFree: 1 kB\n") && !proc_memory_bytes("MemTotal: lots kB\n") &&
        !proc_memory_bytes("MemTotal: 99999999999999999 kB\n"), "meminfo rejects");
    require(proc_uptime_seconds("12345.67 54321.00\n") == 12345ull && !proc_uptime_seconds("soon\n") && !proc_uptime_seconds(""), "uptime");
    ToolPaths paths;
    paths.installed = paths.available = (root / "packages.db").string();
    sqlite3* db{}; require(sqlite3_open(paths.installed.c_str(), &db) == SQLITE_OK, "fixture database");
    require(sqlite3_exec(db, "CREATE TABLE packages(pkg TEXT,version TEXT,comment TEXT); INSERT INTO packages VALUES('curl','1.0','transfer client');", nullptr, nullptr, nullptr) == SQLITE_OK, "fixture schema");
    sqlite3_close(db);
    const auto package_result = execute_tool(s, "mport_search", R"({"query":"curl","source":"installed"})", cancel, paths);
    require(package_result.find("curl | 1.0") != std::string::npos, "package lookup");
    require(execute_tool(s, "mport_search", R"({"query":"' OR 1=1 --","source":"installed"})", cancel, paths).find("No matches") != std::string::npos, "SQL injection");
    rejects([&] { execute_tool(s, "mport_search", R"({"query":"curl","source":"install"})", cancel, paths); });
    rejects([&] { execute_tool(s, "mport_search", R"({"query":"curl","source":"installed","sql":"delete"})", cancel, paths); });
    require(execute_tool(s, "mport_count", R"({"source":"installed"})", cancel, paths) == "Exact installed package count: 1\n", "Exact package count");
    require(sqlite3_open(paths.installed.c_str(), &db) == SQLITE_OK, "Reopen fixture");
    require(sqlite3_exec(db, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<50) INSERT INTO packages SELECT 'fixture-' || x,'1.0','test package' FROM n", nullptr, nullptr, nullptr) == SQLITE_OK, "Large package fixture");
    sqlite3_close(db);
    require(execute_tool(s, "mport_count", R"({"source":"installed"})", cancel, paths) == "Exact installed package count: 51\n", "Count was limited to preview size");
    auto preview = execute_tool(s, "mport_search", R"({"query":"fixture","source":"installed"})", cancel, paths);
    require(preview.starts_with("Exact installed package count: 51\nExact matches for the literal query: 50\nPreview only"), "Missing totals before truncated preview");
    require(preview.find("Showing at most 20") != std::string::npos, "Missing preview limit");
    for (const auto* args : {R"({"query":"*","source":"installed"})", R"({"source":"installed"})", R"({"query":"","source":"installed"})"})
      rejects([&] { execute_tool(s, "mport_search", args, cancel, paths); });
    rejects([&] { execute_tool(s, "mport_count", R"({"source":"installed","query":"*"})", cancel, paths); });
    rejects([&] { execute_tool(s, "mport_count", R"({"source":"install"})", cancel, paths); });
    require(execute_tool(s, "mport_count", R"({"source":"available"})", cancel, paths).find("51") != std::string::npos, "Cached count");
    require(sqlite3_open(paths.installed.c_str(), &db) == SQLITE_OK, "Reopen empty fixture");
    require(sqlite3_exec(db, "DELETE FROM packages", nullptr, nullptr, nullptr) == SQLITE_OK, "Empty fixture"); sqlite3_close(db);
    require(execute_tool(s, "mport_count", R"({"source":"installed"})", cancel, paths) == "Exact installed package count: 0\n", "Empty count");
    auto disabled = s; disabled.package_queries = false;
    rejects([&] { execute_tool(disabled, "mport_count", R"({"source":"installed"})", cancel, paths); });
    require(execute_tool(s, "system_information", "{}", cancel).find("Operating system:") != std::string::npos, "native system information");
    rejects([&] { execute_tool(s, "system_information", R"({"command":"whoami"})", cancel); });
    rejects([&] { execute_tool(s, "file_search", R"({"query":"test"})", cancel); });
    s.file_search = true;
    paths.locate = (root / "locate-fixture").string();
    const auto readable = (root / "readable-file").string(); std::ofstream(readable) << "not read by tool";
    std::ofstream script(paths.locate);
    script << "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" << (root / "arguments").string() << "'\nprintf '%s\\0' '" << readable << "' '/nonexistent-chat-test-path'\n";
    script.close(); chmod(paths.locate.c_str(), 0700);
    auto files = execute_tool(s, "file_search", R"json({"query":"[?]*;$(touch NO)"})json", cancel, paths);
    require(files.find(readable) != std::string::npos && files.find("/nonexistent-chat-test-path") == std::string::npos, "readable filename filtering");
    std::ifstream input(root / "arguments"); std::string arguments((std::istreambuf_iterator<char>(input)), {});
    require(arguments.find("--\n*\\[\\?\\]\\*;$(touch NO)*") != std::string::npos, "literal locate arguments");
    rejects([&] { execute_tool(s, "file_search", R"({"query":""})", cancel, paths); });
    // A blocked child must be terminated and reaped when Stop is requested.
    { std::ofstream slow(paths.locate); slow << "#!/bin/sh\nexec /bin/sleep 30\n"; }
    const auto began = std::chrono::steady_clock::now();
    std::thread stop([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); g_cancellable_cancel(cancel); });
    bool stopped = false;
    try { execute_tool(s, "file_search", R"({"query":"blocked"})", cancel, paths); }
    catch (const std::exception&) { stopped = true; }
    stop.join();
    require(stopped && std::chrono::steady_clock::now() - began < std::chrono::seconds(3), "Blocked tool did not stop promptly");
    g_cancellable_reset(cancel);
    require(search_result_text(R"({"web":{"results":[{"title":"Example","url":"https://example.org/","description":"snippet"}]}})", true).find("https://example.org/") != std::string::npos, "Brave parsing");
    require(search_result_text(R"({"results":[{"title":"Example","url":"https://example.org/","content":"snippet"}]})", false).find("snippet") != std::string::npos, "SearXNG parsing");
    rejects([&] { search_result_text(R"({"results":5})", false); });
    s.web_search = true; s.search_provider = "searxng";
    s.search_url = std::string(argv[1]) + ":" + argv[2] + "/search";
    s.url += "/v1";
    validate_tool_settings(s);
    const auto result = execute_tool(s, "web_search", R"({"query":"test & query"})", cancel);
    require(result.find("https://example.org/") != std::string::npos, "search HTTP transport");
    const auto saved_url = s.search_url;
    s.search_url += "?bad=1"; rejects([&] { validate_tool_settings(s); }); s.search_url = saved_url;
    s.authenticate = true; s.token = "model-secret";
    require(s.tool_rounds == 12 && s.tool_calls == 24 && s.request_timeout == 900, "Default limits");
    for (int invalid : {0, 33}) { auto bad = s; bad.tool_rounds = invalid; rejects([&] { save_settings(root / "invalid.json", bad); }); }
    for (int invalid : {0, 65}) { auto bad = s; bad.tool_calls = invalid; rejects([&] { save_settings(root / "invalid.json", bad); }); }
    for (int invalid : {59, 3601}) { auto bad = s; bad.request_timeout = invalid; rejects([&] { save_settings(root / "invalid.json", bad); }); }
    s.tool_rounds = 17; s.tool_calls = 40; s.request_timeout = 1200;
    save_settings(root / "settings/settings.json", s);
    const auto loaded = load_settings(root / "settings/settings.json");
    require(loaded.file_search && loaded.web_search && loaded.search_url == s.search_url, "tool settings roundtrip");
    require(loaded.tool_rounds == 17 && loaded.tool_calls == 40 && loaded.request_timeout == 1200, "Configured limits not saved");
    s.tool_rounds = 12; s.tool_calls = 24; s.request_timeout = 900;
    t.messages.push_back({"user", "Use tools"});
    t = complete(s, t, cancel);
    require(t.messages.size() == 2 && t.messages.back().tools.size() == 2, "tool loop did not finish");
    save_threads(root / "history/threads.json", {t});
    const auto loaded_threads = load_threads(root / "history/threads.json");
    require(loaded_threads[0].messages[1].tools.size() == 2, "tool history lost");
    t.messages.push_back({"user", "Follow up"}); t = complete(s, t, cancel);
    require(t.messages.back().text == "Verified tools", "follow-up tool history failed");
    // Older tool traces participate in compaction, while summaries cannot run tools.
    auto compacted = t;
    const auto pair = t.messages[1];
    for (int i = 0; i < 6; ++i) {
      compacted.messages.push_back({"user", "Tell me about that result again."});
      compacted.messages.push_back(pair);
    }
    compacted.messages.push_back({"user", "Continue"});
    s.context = 8192;
    compacted = complete(s, compacted, cancel);
    require(compacted.compacted > 0, "Tool context was not compacted");
    Thread pending; pending.id = t.id; pending.messages.push_back({"user", "Use tools"});
    s.file_search = s.system_information = s.package_queries = s.web_search = false;
    rejects([&] { complete(s, pending, cancel); });
    s.web_search = true; s.context = 4096; s.model = "fixture-large";
    auto limited = complete(s, pending, cancel);
    require(limited.messages.back().tools[0].result.find("truncated to fit context") != std::string::npos, "Large tool output not bounded to context");
    // A retained latest turn that nearly fills the budget is summarized to make
    // room for the reply's tool results instead of failing or gutting them.
    Thread crowded; crowded.id = t.id;
    crowded.messages.push_back({"user", "Earlier question"});
    crowded.messages.push_back({"assistant", "Earlier answer"});
    crowded.messages.push_back({"user", "Long question"});
    crowded.messages.push_back({"assistant", std::string(19800, 'a')});
    crowded.messages.push_back({"user", "Use tools"});
    s.context = 8192;
    auto room = complete(s, crowded, cancel);
    require(room.compacted == 4 && room.messages.size() == 6 && room.messages.back().text == "Verified tools" &&
        room.messages.back().tools[0].result.find("truncated") == std::string::npos, "Crowded context did not make room for tool results");
    s.context = 4096;
    s.web_search = false; s.context = 32768;
    s.system_information = true;
    for (const auto* model : {"fixture-loop", "fixture-batch", "fixture-stubborn", "fixture-stubborn-text"}) {
      s.model = model;
      auto bounded = complete(s, pending, cancel);
      const auto& reply = bounded.messages.back().text;
      if (s.model == "fixture-stubborn") require(reply.find("did not provide a final answer") != std::string::npos, "Missing honest fallback for ignored tool_choice");
      else require(reply == (s.model == "fixture-stubborn-text" ? "Partial answer" : "Answer from collected results"), "Tool limit discarded final answer");
      require(bounded.messages.back().tools.size() == (s.model == "fixture-batch" ? 24 : 12), "Tool execution allowance exceeded");
      save_threads(root / "history/long.json", {bounded});
      require(load_threads(root / "history/long.json")[0].messages.back().tools.size() == bounded.messages.back().tools.size(), "Long tool history lost");
      // The allowance belongs to each reply, not the conversation.
      bounded.messages.push_back({"user", "Follow up"});
      s.model = "fixture-unknown";
      bounded = complete(s, bounded, cancel);
      require(bounded.messages.back().text == "Verified tools", "Conversation failed after reaching tool limit");
    }
    s.tool_rounds = 2; s.tool_calls = 3; s.model = "fixture-custom";
    auto custom = complete(s, pending, cancel);
    require(custom.messages.back().tools.size() == 2, "Custom round limit ignored");
    s.tool_rounds = 12; s.tool_calls = 5; s.model = "fixture-custom-calls";
    custom = complete(s, pending, cancel);
    require(custom.messages.back().tools.size() == 5, "Custom call limit or partial batch ignored");
    s.tool_calls = 24;
    s.model = "fixture-unknown";
    auto unknown = complete(s, pending, cancel);
    require(unknown.messages.back().tools[0].result.find("Unknown or disabled") != std::string::npos, "unknown tool not rejected");
    s.web_search = true; s.model = "fixture-repeat";
    auto repeated = complete(s, pending, cancel);
    require(repeated.messages.back().tools.size() == 3 && repeated.messages.back().text == "Used existing search results", "Repeated search did not recover");
    // A web search after a local tool asks first; the search runs only when allowed.
    s.model = "fixture-confirm";
    std::vector<std::string> asked;
    const auto allow = [&](const std::string& query) { asked.push_back(query); return true; };
    const auto deny = [&](const std::string& query) { asked.push_back(query); return false; };
    auto allowed = complete(s, pending, cancel, nullptr, allow);
    require(asked == std::vector<std::string>{"fixture query"} && allowed.messages.back().tools.size() == 2 &&
        allowed.messages.back().tools[1].result.find("https://example.org/") != std::string::npos &&
        allowed.messages.back().text == "Confirmed flow", "Allowed search did not run");
    auto denied = complete(s, pending, cancel, nullptr, deny);
    require(asked.size() == 2 && denied.messages.back().tools.size() == 2 &&
        denied.messages.back().tools[1].result.find("declined by the user") != std::string::npos &&
        denied.messages.back().text == "Confirmed flow", "Declined search was not recorded");
    // Without preceding local results, no confirmation is requested.
    s.model = "fixture-repeat";
    asked.clear(); complete(s, pending, cancel, nullptr, deny);
    require(asked.empty(), "Confirmation asked without local tool results");
    g_cancellable_cancel(cancel); rejects([&] { execute_tool(s, "system_information", "{}", cancel); });
    g_object_unref(cancel);
    std::cout << "PASS: tool schemas, argument validation, native system data, read-only package queries, literal filename search, search HTTP, tool loop/history, disabled/unknown tools and limits\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
