#include "audio.hpp"

#include <sys/ioctl.h>
#include <sys/soundcard.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace midnight {
namespace {
constexpr char mixer_path[] = "/dev/mixer";

std::string error(const char* action) {
  return std::string(action) + ": " + std::strerror(errno);
}

int clamp_level(int level) { return std::clamp(level, 0, 100); }
} // namespace

std::expected<VolumeControl::Levels, std::string> VolumeControl::read_levels() const {
  const int fd = open(mixer_path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::unexpected(error("Open mixer"));

  int mask = 0;
  if (ioctl(fd, SOUND_MIXER_READ_DEVMASK, &mask) < 0) {
    const auto message = error("Read mixer controls");
    close(fd);
    return std::unexpected(message);
  }
  const int device = (mask & SOUND_MASK_VOLUME) ? SOUND_MIXER_VOLUME :
                     (mask & SOUND_MASK_PCM) ? SOUND_MIXER_PCM : -1;
  if (device < 0) {
    close(fd);
    return std::unexpected("Mixer has no master or PCM playback control");
  }

  int packed = 0;
  const int result = ioctl(fd, device == SOUND_MIXER_VOLUME ? SOUND_MIXER_READ_VOLUME : SOUND_MIXER_READ_PCM, &packed);
  if (result < 0) {
    const auto message = error("Read volume");
    close(fd);
    return std::unexpected(message);
  }
  close(fd);
  return Levels{device, clamp_level(packed & 0xff), clamp_level((packed >> 8) & 0xff)};
}

std::expected<void, std::string> VolumeControl::write_levels(const Levels& levels) const {
  const int fd = open(mixer_path, O_RDWR | O_CLOEXEC);
  if (fd < 0) return std::unexpected(error("Open mixer"));
  int packed = clamp_level(levels.left) | (clamp_level(levels.right) << 8);
  const int result = ioctl(fd, levels.device == SOUND_MIXER_VOLUME ? SOUND_MIXER_WRITE_VOLUME : SOUND_MIXER_WRITE_PCM,
                           &packed);
  if (result < 0) {
    const auto message = error("Set volume");
    close(fd);
    return std::unexpected(message);
  }
  close(fd);
  return {};
}

VolumeState VolumeControl::state(const Levels& levels) {
  if (levels.left > 0 || levels.right > 0) {
    saved_left_ = levels.left;
    saved_right_ = levels.right;
  }
  return {(levels.left + levels.right) / 2, levels.left == 0 && levels.right == 0};
}

std::expected<VolumeState, std::string> VolumeControl::read() {
  auto levels = read_levels();
  if (!levels) return std::unexpected(levels.error());
  return state(*levels);
}

std::expected<VolumeState, std::string> VolumeControl::adjust(int change) {
  auto levels = read_levels();
  if (!levels) return std::unexpected(levels.error());
  (void)state(*levels);
  if (levels->left == 0 && levels->right == 0 && change > 0) {
    levels->left = saved_left_;
    levels->right = saved_right_;
  }
  levels->left = clamp_level(levels->left + change);
  levels->right = clamp_level(levels->right + change);
  if (auto written = write_levels(*levels); !written) return std::unexpected(written.error());
  return read();
}

std::expected<VolumeState, std::string> VolumeControl::toggle_mute() {
  auto levels = read_levels();
  if (!levels) return std::unexpected(levels.error());
  if (levels->left == 0 && levels->right == 0) {
    levels->left = saved_left_;
    levels->right = saved_right_;
  } else {
    (void)state(*levels);
    levels->left = 0;
    levels->right = 0;
  }
  if (auto written = write_levels(*levels); !written) return std::unexpected(written.error());
  return read();
}

} // namespace midnight
