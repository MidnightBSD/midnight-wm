// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "application_session.hpp"
#include "config.hpp"
#include <glib.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

int main() {
  char temporary[] = "/tmp/midnight-applications-test-XXXXXX";
  if (!mkdtemp(temporary)) return 1;
  const std::filesystem::path root(temporary), path = root / "state/applications.ini";
  midnight::ApplicationSessionStore store(path);
  bool passed = true;
  const auto check = [&](bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; passed = false; }
  };
  const auto read = [&] {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  auto loaded = store.load();
  check(loaded && !*loaded, "missing checkpoint must be fresh");
  midnight::ApplicationCheckpoint checkpoint{{{"client-id", "program", {
    "/usr/local/bin/program", "", "literal $(not-executed); with spaces\nand newline",
    std::string("non-UTF8-\xff", 10)}, "/tmp/working directory", midnight::RestartStyle::if_running}}};
  checkpoint.windows = {
    {"client-id", std::string("editor\nbyte-\xff", 13), {-1200, 90, 800, 600}, 3, true, false, false, true},
    {"client-id", "preview", {100, 80, 400, 300}, 1, false, false, true, false}};
  check(bool(store.save(checkpoint)), "checkpoint save failed");
  loaded = store.load();
  check(loaded && *loaded && **loaded == checkpoint, "argument bytes or boundaries changed");
  const auto original = read();
  struct stat st{};
  check(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "checkpoint not private");
  check(stat(path.parent_path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0700, "directory not private");
  // Version 1 checkpoints remain readable and gain an empty placement list.
  auto* legacy = g_key_file_new();
  check(g_key_file_load_from_data(legacy, original.data(), original.size(), G_KEY_FILE_NONE, nullptr), "cannot prepare legacy fixture");
  g_key_file_set_integer(legacy, "Applications", "version", 1);
  g_key_file_remove_key(legacy, "Applications", "windows", nullptr);
  g_key_file_remove_group(legacy, "Window 0", nullptr);
  g_key_file_remove_group(legacy, "Window 1", nullptr);
  gsize legacy_length{};
  char* legacy_text = g_key_file_to_data(legacy, &legacy_length, nullptr);
  std::ofstream(path, std::ios::trunc).write(legacy_text, legacy_length);
  g_free(legacy_text); g_key_file_unref(legacy);
  loaded = store.load();
  check(loaded && *loaded && (**loaded).applications == checkpoint.applications && (**loaded).windows.empty(), "version 1 compatibility failed");
  check(bool(store.save(checkpoint)), "upgrade to placement format failed");
  auto invalid = checkpoint;
  invalid.windows[0].client_id = "not-in-checkpoint";
  check(!store.save(invalid) && read() == original, "orphan window accepted");
  invalid = checkpoint; invalid.windows.push_back(invalid.windows[0]);
  check(!store.save(invalid), "ambiguous window role accepted");
  invalid = checkpoint; invalid.windows[0].role = std::string("bad\0role", 8);
  check(!store.save(invalid), "NUL in role accepted");
  invalid = checkpoint; invalid.windows[0].role.resize(4097, 'x');
  check(!store.save(invalid), "unbounded role accepted");
  invalid = checkpoint; invalid.windows[0].workspace = 4;
  check(!store.save(invalid), "invalid workspace accepted");
  invalid = checkpoint; invalid.windows[0].geometry.width = 0;
  check(!store.save(invalid), "empty window geometry accepted");
  invalid = checkpoint; invalid.windows[0].geometry.x = -32769;
  check(!store.save(invalid), "unbounded window geometry accepted");
  invalid = checkpoint; invalid.windows[0].fullscreen = true;
  check(!store.save(invalid), "fullscreen and shaded window accepted");
  invalid = checkpoint; invalid.windows.resize(513, checkpoint.windows[0]);
  check(!store.save(invalid), "unbounded window count accepted");
  invalid = checkpoint;
  invalid.applications.push_back(invalid.applications.front());
  check(!store.save(invalid) && read() == original, "duplicate IDs replaced valid state");
  invalid = checkpoint; invalid.applications[0].arguments[0] = "relative/program";
  check(!store.save(invalid), "relative executable accepted");
  invalid = checkpoint; invalid.applications[0].style = midnight::RestartStyle::never;
  check(!store.save(invalid), "RestartNever persisted");
  invalid = checkpoint; invalid.applications[0].arguments.push_back(std::string("bad\0argument", 12));
  check(!store.save(invalid), "NUL accepted");
  invalid = checkpoint; invalid.applications.resize(129, checkpoint.applications[0]);
  check(!store.save(invalid), "unbounded applications accepted");
  std::ofstream(path.parent_path() / ".session-interrupted") << "partial";
  loaded = store.load();
  check(loaded && *loaded && **loaded == checkpoint, "interrupted write changed checkpoint");
  auto malformed = original;
  malformed.replace(malformed.find("version=2"), 9, "version=9");
  auto bad_encoding = original;
  bad_encoding.insert(bad_encoding.find("client_id=") + 10, "!");
  auto missing_windows = original;
  missing_windows.erase(missing_windows.find("windows=2\n"), std::string("windows=2\n").size());
  auto bad_states = original;
  bad_states.replace(bad_states.find("states=9"), 8, "states=16");
  for (const auto& bad : {std::string("garbage"), std::string("[Applications]\nversion=1\ncount=1\n"),
      malformed, bad_encoding, missing_windows, bad_states, std::string(1024 * 1024 + 1, 'x')}) {
    std::ofstream(path, std::ios::trunc) << bad;
    check(!store.load() && !store.save(checkpoint) && read() == bad, "invalid checkpoint overwritten or accepted");
  }
  check(bool(store.clear()) && bool(store.save(checkpoint)), "clear did not recover invalid state");
  chmod(path.c_str(), 0644);
  check(!store.load() && !store.save(checkpoint), "public file accepted or overwritten");
  chmod(path.c_str(), 0600);
  chmod(path.parent_path().c_str(), 0755);
  check(!store.load() && !store.save(checkpoint), "public directory accepted");
  chmod(path.parent_path().c_str(), 0700);
  std::filesystem::rename(path, root / "target");
  std::filesystem::create_symlink(root / "target", path);
  check(!store.load() && !store.save(checkpoint), "symlink accepted or overwritten");
  check(bool(store.clear()) && std::filesystem::exists(root / "target"), "clear followed symlink");
  check(bool(store.save(checkpoint)), "save after symlink clear failed");
  check(bool(store.clear()) && bool(store.clear()), "clear not idempotent");
  std::filesystem::create_directory(path);
  check(!store.save(checkpoint) && std::filesystem::is_directory(path), "failed save damaged destination");
  const auto defaults = midnight::parse_config("");
  const auto enabled = midnight::parse_config("[Session]\nsave_application_checkpoints=true\n");
  check(defaults && !defaults->save_application_checkpoints && enabled && enabled->save_application_checkpoints,
        "application saving must be explicit opt-in");
  std::filesystem::remove_all(root);
  return passed ? 0 : 1;
}
