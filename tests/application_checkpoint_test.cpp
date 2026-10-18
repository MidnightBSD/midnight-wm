// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "application_session.hpp"
#include "config.hpp"
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
  check(bool(store.save(checkpoint)), "checkpoint save failed");
  loaded = store.load();
  check(loaded && *loaded && **loaded == checkpoint, "argument bytes or boundaries changed");
  const auto original = read();
  struct stat st{};
  check(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "checkpoint not private");
  check(stat(path.parent_path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0700, "directory not private");
  auto invalid = checkpoint;
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
  malformed.replace(malformed.find("version=1"), 9, "version=9");
  auto bad_encoding = original;
  bad_encoding.insert(bad_encoding.find("client_id=") + 10, "!");
  for (const auto& bad : {std::string("garbage"), std::string("[Applications]\nversion=1\ncount=1\n"),
      malformed, bad_encoding, std::string(1024 * 1024 + 1, 'x')}) {
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
