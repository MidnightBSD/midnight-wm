// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "chat_core.hpp"
#include "private_state.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
using namespace midnight::chat;
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
  bool rejected = false;
  try { f(); } catch (const std::exception&) { rejected = true; }
  require(rejected, "Expected rejection");
}
int main(int argc, char** argv) {
  try {
    require(argc >= 4, "usage: chat_test base-url port temporary-directory [live-model]");
    require(estimate_tokens("") == 0 && estimate_tokens("Hello") == 2 && estimate_tokens("Hello world.") == 6 &&
        estimate_tokens("海") == 3 && estimate_tokens(std::string(100, ' ')) == 100, "Token estimate");
    require(compaction_target(800, 1000) == 0 && compaction_target(801, 1000) == 600 && compaction_target(951, 1000) == 400,
        "Compaction policy");
    Settings s; s.url = argv[1]; s.port = std::stoi(argv[2]); s.model = argc > 4 ? argv[4] : "fixture";
    const std::filesystem::path root(argv[3]);
    auto* cancel = g_cancellable_new();
    const auto available = models(s, cancel);
    require(std::find(available.begin(), available.end(), s.model) != available.end(), "Model missing from discovery");
    std::cout << "Discovered " << available.size() << " models; selected " << s.model << std::endl;
    Thread t; t.id = "1afdb7f8-dff7-414a-b2bc-e0a56f2e8aa4";
    t.messages.push_back({"user", "My name is Robin. Please greet me in one short sentence."});
    s.reply = 128;
    t = complete(s, t, cancel);
    std::cout << "First reply: " << t.messages.back().text << std::endl;
    t.messages.push_back({"user", "What name did I tell you? Answer with just the name."});
    t = complete(s, t, cancel);
    std::cout << "Context reply: " << t.messages.back().text << std::endl;
    require(t.messages.back().text.find("Robin") != std::string::npos, "Conversation context lost");
    save_settings(root / "config/settings.json", s);
    require(load_settings(root / "config/settings.json").model == s.model, "Settings roundtrip");
    s.context = 2048;
    t.messages.clear();
    for (int i = 0; i < 7; ++i) {
      t.messages.push_back({"user", "My name is Robin. I enjoy walking by the sea. " + std::string(100, ' ')});
      t.messages.push_back({"assistant", "Hello Robin. Walking by the sea sounds relaxing. " + std::string(100, ' ')});
    }
    t.messages.push_back({"user", "What is my name? Answer briefly."});
    t = complete(s, t, cancel);
    require(t.compacted > 0 && !t.summary.empty(), "Compaction did not run");
    require(t.messages.size() == 16, "Compaction discarded transcript");
    std::cout << "Compacted " << t.compacted << " messages; reply: " << t.messages.back().text << std::endl;
    save_threads(root / "data/threads.json", {t});
    const auto restored = load_threads(root / "data/threads.json");
    require(restored.size() == 1 && restored[0].summary == t.summary && restored[0].messages.size() == 16, "History roundtrip");
    require(t.summary_model == s.model && t.summary_context == 2048 && restored[0].summary_model == s.model &&
        restored[0].summary_context == 2048, "Summary metadata roundtrip");
    struct stat st{};
    require(stat((root / "data/threads.json").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "History permissions");
    if (argc > 4) { g_object_unref(cancel); return 0; }
    // Discovery is scoped to the selected model and can only lower the cap.
    auto discovered = s; discovered.context = 16384; discovered.model = "fixture-running";
    auto limit = context_limit(discovered, cancel);
    require(limit.tokens == 3072 && limit.source == "Ollama running model", "Runtime allocation not detected");
    discovered.context = 2048;
    require(context_limit(discovered, cancel).tokens == 2048, "Discovery raised the configured cap");
    discovered.context = 16384; discovered.model = "fixture-metadata";
    require(context_limit(discovered, cancel).tokens == 4096, "Gateway metadata not detected");
    discovered.model = "fixture-invalid";
    require(context_limit(discovered, cancel).tokens == 16384, "Malformed metadata accepted");
    discovered.model = "fixture-unloaded";
    require(context_limit(discovered, cancel).tokens == 16384, "Unrelated running model affected limit");
    discovered.model = "fixture-running";
    discovered.url = std::string(argv[1]);
    discovered.url.insert(discovered.url.rfind("/v1"), "/proxy");
    require(context_limit(discovered, cancel).tokens == 3072, "Reverse proxy prefix lost");
    // Repeated discovery for one server and model is answered from the cache;
    // the fixture counts the /proxy/api/ps requests. Forgetting probes again.
    require(context_limit(discovered, cancel).tokens == 3072 && context_limit(discovered, cancel).tokens == 3072, "Cached discovery");
    forget_context_limits();
    require(context_limit(discovered, cancel).tokens == 3072, "Discovery after cache reset");
    // A llama.cpp /tokenize route calibrates the estimate: prose padded with
    // whitespace that compacts under the local estimate fits once measured.
    Thread padded; padded.id = t.id;
    for (int i = 0; i < 6; ++i) {
      padded.messages.push_back({"user", "My name is Robin. I enjoy walking by the sea. " + std::string(100, ' ')});
      padded.messages.push_back({"assistant", "Hello Robin. Walking by the sea sounds relaxing. " + std::string(100, ' ')});
    }
    padded.messages.push_back({"user", "What is my name? Answer briefly."});
    ContextInfo local_info;
    require(complete(s, padded, cancel, &local_info).compacted > 0 && local_info.tokenizer == "local estimate" && local_info.used > 0,
        "Local estimate did not compact padded prose");
    auto calibrated = s; calibrated.url.insert(calibrated.url.rfind("/v1"), "/tokenizer");
    ContextInfo measured_info;
    require(complete(calibrated, padded, cancel, &measured_info).compacted == 0 && measured_info.tokenizer == "server tokenizer",
        "Server tokenizer did not calibrate the estimate");
    Thread early; early.id = t.id;
    for (int i = 0; i < 3; ++i) {
      early.messages.push_back({"user", std::string(480, 'u')});
      early.messages.push_back({"assistant", std::string(480, 'a')});
    }
    early.messages.push_back({"user", "Continue, Robin"});
    auto small = s; small.context = 8192; small.model = "fixture-small";
    ContextInfo info;
    auto packed = complete(small, early, cancel, &info);
    require(info.tokens == 2048 && packed.compacted > 0, "Did not compact before the discovered limit");
    require(packed.compacted <= 4 && packed.messages[4].text == early.messages[4].text,
        "Compaction failed to preserve newest complete turn");
    // This input fits the hard budget but exceeds the early trigger.
    auto configured_small = s; configured_small.context = 2048;
    require(complete(configured_small, early, cancel).compacted > 0, "Early compaction trigger failed");
    auto manual = early; manual.messages.pop_back();
    std::vector<std::pair<std::size_t, std::size_t>> progress;
    auto compacted = compact(s, manual, cancel, nullptr, [&](std::size_t done, std::size_t total) { progress.emplace_back(done, total); });
    require(compacted.compacted == 4 && compacted.messages.size() == manual.messages.size() &&
        compacted.messages.back().text == manual.messages.back().text, "Manual compaction lost latest turn/transcript");
    require(progress.size() == 2 && progress.back() == std::pair<std::size_t, std::size_t>(4, 6), "Compaction progress not reported");
    require(compacted.summary_model == s.model && compacted.summary_context == 2048, "Summary metadata missing");
    // A summary written for a larger window is condensed instead of failing.
    auto oversized = manual; oversized.summary = std::string(6000, 'S'); oversized.compacted = 2;
    const auto condensed = compact(s, oversized, cancel);
    require(condensed.compacted == 4 && condensed.summary.size() < 100, "Oversized summary was not condensed");
    // An old turn larger than the entire window must be folded in UTF-8 safe chunks.
    auto huge = early;
    huge.messages[0].text.clear();
    for (int i = 0; i < 1800; ++i) huge.messages[0].text += "海";
    huge.messages[1].tools.push_back({"old_tool", "system_information", "{}", std::string(3000, 'T')});
    const auto original = huge.messages[0].text;
    auto chunked = complete(small, huge, cancel);
    require(chunked.compacted >= 2 && chunked.messages[0].text == original &&
        chunked.messages[1].tools[0].result.size() == 3000, "Oversized old turn was lost or not compacted");
    auto stopping = s; stopping.model = "fixture-cancel";
    std::thread compact_stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); g_cancellable_cancel(cancel); });
    rejects([&] { compact(stopping, manual, cancel); }); compact_stopper.join();
    require(manual.compacted == 0 && manual.summary.empty(), "Cancelled compaction changed original history");
    g_cancellable_reset(cancel);
    stopping.model = "fixture-fail";
    rejects([&] { compact(stopping, manual, cancel); });
    require(manual.compacted == 0 && manual.summary.empty(), "Failed compaction changed original history");
    auto legacy = s; legacy.authenticate = true; legacy.token = "legacy-secret";
    const auto profile_path = root / "profiles/settings.json";
    save_settings(profile_path, legacy);
    const auto legacy_bytes = midnight::read_private_state(profile_path, 16 * 1024 * 1024);
    auto profiles = load_server_profiles(profile_path);
    require(profiles.servers.size() == 1 && profiles.selected == profiles.servers[0].id &&
        profiles.servers[0].settings.token == legacy.token && profiles.servers[0].settings.model == legacy.model,
        "Legacy server migration lost settings");
    require(midnight::read_private_state(profile_path, 16 * 1024 * 1024) == legacy_bytes,
        "Loading legacy settings rewrote the file");
    auto second = new_server_profile("Second server");
    require(!second.settings.authenticate && second.settings.token.empty() && !second.settings.system_information,
        "New server inherited credentials or tool permissions");
    second.settings.url = "http://localhost/other/v1"; second.settings.port = 8080;
    second.settings.model = "second-model"; second.settings.authenticate = true; second.settings.token = "second-secret";
    second.settings.context = 16384; second.settings.system_information = true;
    profiles.selected = second.id; profiles.servers.push_back(second);
    save_server_profiles(profile_path, profiles);
    const auto reloaded_profiles = load_server_profiles(profile_path);
    require(reloaded_profiles.selected == second.id && reloaded_profiles.servers.size() == 2 &&
        reloaded_profiles.servers[0].settings.token == "legacy-secret" &&
        reloaded_profiles.servers[1].settings.token == "second-secret" &&
        reloaded_profiles.servers[1].settings.port == 8080 && reloaded_profiles.servers[1].settings.model == "second-model" &&
        reloaded_profiles.servers[1].settings.context == 16384 && reloaded_profiles.servers[1].settings.system_information,
        "Server profile roundtrip lost or mixed settings");
    const auto profile_bytes = midnight::read_private_state(profile_path, 16 * 1024 * 1024);
    auto invalid = profiles; invalid.selected = "missing";
    rejects([&] { save_server_profiles(profile_path, invalid); });
    invalid = profiles; invalid.servers[1].id = invalid.servers[0].id;
    rejects([&] { save_server_profiles(profile_path, invalid); });
    invalid = profiles; invalid.servers[1].name = invalid.servers[0].name;
    rejects([&] { save_server_profiles(profile_path, invalid); });
    invalid = profiles; invalid.servers[1].name = "\n";
    rejects([&] { save_server_profiles(profile_path, invalid); });
    invalid = profiles; invalid.servers.clear();
    rejects([&] { save_server_profiles(profile_path, invalid); });
    invalid = profiles;
    while (invalid.servers.size() <= max_servers)
      invalid.servers.push_back(new_server_profile("Server " + std::to_string(invalid.servers.size())));
    rejects([&] { save_server_profiles(profile_path, invalid); });
    require(midnight::read_private_state(profile_path, 16 * 1024 * 1024) == profile_bytes,
        "Invalid profile save overwrote the previous settings");
    require(stat(profile_path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "Profile permissions");
    // The final reply at the boundary must remain readable. A further pending
    // turn must be rejected before either storage or generation changes it.
    Thread boundary; boundary.id = t.id;
    for (std::size_t i = 0; i < max_thread_messages - 1; ++i)
      boundary.messages.push_back({i % 2 ? "assistant" : "user", "x"});
    boundary.compacted = max_thread_messages - 2;
    boundary.summary = "Earlier conversation.";
    boundary = complete(s, std::move(boundary), cancel);
    require(boundary.messages.size() == max_thread_messages, "Final allowed reply was rejected");
    const auto boundary_path = root / "data/boundary.json";
    save_threads(boundary_path, {boundary});
    require(load_threads(boundary_path)[0].messages.size() == max_thread_messages, "Boundary history cannot reload");
    const auto saved_boundary = midnight::read_private_state(boundary_path, 16 * 1024 * 1024);
    require(saved_boundary && saved_boundary->has_value(), "Read boundary snapshot");
    boundary.messages.push_back({"user", "Another turn"});
    rejects([&] { save_threads(boundary_path, {boundary}); });
    const auto unchanged = midnight::read_private_state(boundary_path, 16 * 1024 * 1024);
    require(unchanged && *unchanged == *saved_boundary, "Rejected save overwrote valid history");
    rejects([&] { complete(s, boundary, cancel); });
    s.authenticate = true; s.token = "fixture-secret";
    require(!models(s, cancel).empty(), "Authenticated discovery");
    t.messages.push_back({"user", "Say hello"}); t = complete(s, t, cancel);
    s.token = "bad\r\nheader"; rejects([&] { models(s, cancel); });
    s.authenticate = false; s.token.clear();
    for (const auto* path : {"/unauthorized", "/malformed", "/oversized", "/redirect", "/wrong-shape"}) {
      s.url = std::string(argv[1]) + path;
      rejects([&] { models(s, cancel); });
    }
    s.url = argv[1];
    t.messages.push_back({"user", std::string(8192, 'x')});
    rejects([&] { complete(s, t, cancel); });
    g_cancellable_cancel(cancel); rejects([&] { models(s, cancel); });
    g_cancellable_reset(cancel);
    s.url = std::string(argv[1]) + "/slow";
    std::thread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); g_cancellable_cancel(cancel); });
    rejects([&] { models(s, cancel); }); stopper.join();
    // Oversized fields are clamped on save so the history always reloads.
    Thread clamped; clamped.id = t.id; clamped.title = std::string(300, 't'); clamped.summary = std::string(max_summary_bytes + 1, 's');
    clamped.messages.push_back({"user", "run a tool"});
    Message reply{"assistant", std::string(max_message_bytes + 10, 'a')};
    reply.tools.push_back({"call_1", "file_search", R"({"query":"x"})", std::string(20000, 'r') + "é"});
    clamped.messages.push_back(std::move(reply));
    save_threads(root / "data/clamped.json", {clamped});
    const auto reloaded = load_threads(root / "data/clamped.json");
    require(reloaded.size() == 1 && reloaded[0].title.size() <= max_title_bytes && reloaded[0].summary.size() <= max_summary_bytes &&
        reloaded[0].messages[1].text.size() <= max_message_bytes, "Clamped fields did not reload");
    const auto& saved_result = reloaded[0].messages[1].tools[0].result;
    require(saved_result.size() <= max_tool_result_bytes && saved_result.ends_with("[Tool output truncated for storage.]") &&
        g_utf8_validate(saved_result.c_str(), -1, nullptr), "Tool result clamp");
    Settings plain; plain.authenticate = true; plain.token = "secret";
    for (const auto* url : {"http://localhost/v1", "http://LOCALHOST:11434/v1", "http://127.0.0.1/v1", "http://[::1]/v1", "http://app.localhost/v1", "https://example.com/v1"}) {
      plain.url = url; require(!cleartext_credentials(plain), "Loopback or TLS endpoint flagged as cleartext");
    }
    for (const auto* url : {"http://example.com/v1", "http://192.168.1.5:8080/v1", "http://[fd00::1]/v1"}) {
      plain.url = url; require(cleartext_credentials(plain), "Remote HTTP endpoint with token not flagged");
    }
    plain.authenticate = false; require(!cleartext_credentials(plain), "Token off still flagged");
    require(midnight::write_private_state(root / "data/bad.json", "{broken").has_value(), "Write malformed fixture");
    rejects([&] { load_threads(root / "data/bad.json"); });
    chmod((root / "config/settings.json").c_str(), 0644);
    rejects([&] { load_settings(root / "config/settings.json"); });
    g_object_unref(cancel);
    std::cout << "PASS: discovery, authentication, context, compaction, storage, malformed/oversized responses, redirects and cancellation\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
