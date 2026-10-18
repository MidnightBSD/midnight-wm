// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace midnight {
// Logical coordinates, independent of the display backend and UI scale.
struct SessionGeometry {
  int x{}, y{}, width{620}, height{430};
  bool operator==(const SessionGeometry&) const = default;
};
struct SessionFolder {
  std::string path;
  SessionGeometry geometry;
  std::uint32_t workspace{};
  bool maximized{}, shaded{};
  bool operator==(const SessionFolder&) const = default;
};
struct SessionSnapshot {
  std::uint32_t workspace{};
  std::vector<SessionFolder> folders;
  bool operator==(const SessionSnapshot&) const = default;
};
// Areas exclude panels. Recover onto the nearest area and keep the whole folder
// reachable, even when a saved monitor has disappeared.
SessionGeometry recover_session_geometry(SessionGeometry saved,
                                        const std::vector<SessionGeometry>& areas);

class SessionStore {
public:
  explicit SessionStore(std::filesystem::path path = default_path());
  static std::filesystem::path default_path();
  std::expected<std::optional<SessionSnapshot>, std::string> load() const;
  std::expected<void, std::string> save(const SessionSnapshot& snapshot) const;
  std::expected<void, std::string> clear() const;
private:
  std::filesystem::path path_;
};
} // namespace midnight
