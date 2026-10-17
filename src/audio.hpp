#pragma once

#include <expected>
#include <string>

namespace midnight {

struct VolumeState {
  int percent{};
  bool muted{};
  bool operator==(const VolumeState&) const = default;
};

// Controls the default OSS mixer. Muting saves the stereo levels and sets both
// channels to zero so it also works with mixers without a mute switch.
class VolumeControl {
public:
  [[nodiscard]] std::expected<VolumeState, std::string> read();
  [[nodiscard]] std::expected<VolumeState, std::string> adjust(int change);
  [[nodiscard]] std::expected<VolumeState, std::string> set_percent(int percent);
  [[nodiscard]] std::expected<VolumeState, std::string> toggle_mute();

private:
  struct Levels {
    int device{};
    int left{};
    int right{};
  };

  [[nodiscard]] std::expected<Levels, std::string> read_levels() const;
  [[nodiscard]] std::expected<void, std::string> write_levels(const Levels& levels) const;
  [[nodiscard]] VolumeState state(const Levels& levels);

  int saved_left_{50};
  int saved_right_{50};
};

} // namespace midnight
