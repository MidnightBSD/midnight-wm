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
    struct stat st{};
    require(stat((root / "data/threads.json").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "History permissions");
    if (argc > 4) { g_object_unref(cancel); return 0; }
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
    require(midnight::write_private_state(root / "data/bad.json", "{broken").has_value(), "Write malformed fixture");
    rejects([&] { load_threads(root / "data/bad.json"); });
    chmod((root / "config/settings.json").c_str(), 0644);
    rejects([&] { load_settings(root / "config/settings.json"); });
    g_object_unref(cancel);
    std::cout << "PASS: discovery, authentication, context, compaction, storage, malformed/oversized responses, redirects and cancellation\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
