// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <memory>
#include <string>
#include <vector>
namespace midnight {
struct MediaPlayerState {
  std::string bus_name, title, artist, status;
  bool can_control{}, can_play{}, can_pause{}, can_previous{}, can_next{};
  bool operator==(const MediaPlayerState&) const = default;
};
// MPRIS transport and discovery, independent of the desktop presentation backend.
class MediaPlayers {
public:
  MediaPlayers();
  ~MediaPlayers();
  MediaPlayers(const MediaPlayers&) = delete;
  MediaPlayers& operator=(const MediaPlayers&) = delete;
  void initialize();
  [[nodiscard]] std::vector<MediaPlayerState> players() const;
  void command(const std::string& bus_name, const std::string& method);
private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}
