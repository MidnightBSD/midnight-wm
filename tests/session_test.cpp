// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "session.hpp"
#include "config.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

int main() {
  char temporary[] = "/tmp/midnight-session-test-XXXXXX";
  if (!mkdtemp(temporary)) return 1;
  const std::filesystem::path root(temporary), path = root / "state/session.ini";
  bool passed = true;
  const auto check = [&](bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; passed = false; }
  };
  const auto read = [&] {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  midnight::SessionStore store(path);
  auto missing = store.load();
  check(missing && !*missing, "missing snapshot should start fresh");
  midnight::SessionSnapshot snapshot{2, {
    {(root / "Folder with spaces\nand newline").string(), {120, 70, 620, 430}, 1, true, false},
    {"trash:///", {-200, 100, 500, 300}, 3, false, true}}};
  check(bool(store.save(snapshot)), "snapshot save failed");
  auto saved = store.load();
  check(saved && *saved && **saved == snapshot, "snapshot did not round trip");
  struct stat st{};
  check(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "snapshot must be private");
  check(stat(path.parent_path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0700, "directory must be private");
  const auto original = read();
  auto invalid = snapshot;
  invalid.workspace = 4;
  check(!store.save(invalid) && read() == original, "invalid save replaced a valid snapshot");
  invalid = snapshot; invalid.folders[0].path = "relative/path";
  check(!store.save(invalid), "relative folder path accepted");
  invalid.folders[0].path = std::string("/bad\0path", 9);
  check(!store.save(invalid), "embedded NUL path accepted");
  invalid = snapshot; invalid.folders.resize(129, snapshot.folders[0]);
  check(!store.save(invalid), "unbounded folder list accepted");
  invalid = snapshot; invalid.folders[0].geometry.width = 0;
  check(!store.save(invalid), "empty geometry accepted");
  // An interrupted write leaves the published snapshot untouched.
  std::ofstream(path.parent_path() / ".session-interrupted") << "partial data";
  saved = store.load();
  check(saved && *saved && **saved == snapshot, "temporary file affected published snapshot");
  for (const auto& bad : {std::string("[Session]\nversion=2\n"),
      std::string("[Session]\nversion=1\nworkspace=0\nfolders=1\n[Folder 0]\npath=/tmp\n"),
      std::string("garbage"), std::string(1024 * 1024 + 1, 'x')}) {
    std::ofstream(path) << bad;
    check(!store.load(), "malformed, incomplete or oversized snapshot accepted");
  }
  check(bool(store.save(snapshot)), "atomic replacement failed");
  chmod(path.c_str(), 0644);
  check(!store.load(), "public snapshot accepted");
  chmod(path.c_str(), 0600);
  chmod(path.parent_path().c_str(), 0755);
  check(!store.load() && !store.save(snapshot), "public session directory accepted");
  chmod(path.parent_path().c_str(), 0700);
  std::filesystem::rename(path, root / "target");
  std::filesystem::create_symlink(root / "target", path);
  check(!store.load(), "snapshot symlink followed");
  check(bool(store.save(snapshot)) && !std::filesystem::is_symlink(path), "atomic save followed a symlink");
  std::ifstream target(root / "target");
  check(std::string(std::istreambuf_iterator<char>(target), {}) == original, "symlink target changed");
  check(bool(store.clear()), "snapshot clear failed");
  missing = store.load();
  check(missing && !*missing && bool(store.clear()), "clear should be idempotent");
  std::filesystem::create_directory(path);
  check(!store.save(snapshot) && std::filesystem::is_directory(path), "failed rename damaged destination");
  for (const auto& entry : std::filesystem::directory_iterator(path.parent_path()))
    check(entry.path().filename() == "session.ini" || entry.path().filename() == ".session-interrupted",
          "failed write leaked a temporary file");

  using midnight::SessionGeometry;
  using midnight::recover_session_geometry;
  for (const auto area : {SessionGeometry{0, 30, 1280, 690}, {0, 30, 1920, 1050},
       {0, 30, 2048, 1122}, {0, 30, 2752, 1122}, {0, 30, 1920, 1050}}) {
    const auto geometry = recover_session_geometry({3000, 1500, 2500, 1400}, {area});
    check(geometry.x >= area.x && geometry.y >= area.y &&
          geometry.x + geometry.width <= area.x + area.width &&
          geometry.y + geometry.height <= area.y + area.height, "geometry recovery left window offscreen");
  }
  const SessionGeometry left{-1280, 30, 1280, 690}, right{0, 0, 1920, 1080};
  check(recover_session_geometry({-1000, 100, 620, 430}, {left, right}).x == -1000,
        "visible window moved off its monitor");
  check(recover_session_geometry({3000, 100, 620, 430}, {left, right}).x == 1300,
        "removed monitor did not recover onto nearest monitor");
  const auto tiny = recover_session_geometry({-10, -10, 620, 430}, {{0, 0, 100, 100}});
  check(tiny == SessionGeometry{0, 0, 100, 100}, "small workarea violated clamp bounds");
  const auto config = midnight::parse_config("[Session]\nrestore_folders=true\nsave_on_logout=true\n");
  check(config && config->restore_session && config->save_session, "session preferences not parsed");
  const auto defaults = midnight::parse_config("");
  check(defaults && !defaults->restore_session && !defaults->save_session, "session must default to fresh startup");
  std::filesystem::remove_all(root);
  if (passed) std::cout << "Session snapshots, privacy, validation, atomic writes and geometry recovery passed\n";
  return passed ? 0 : 1;
}
