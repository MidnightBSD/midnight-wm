// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include "backlight.hpp"

#include <sys/ioctl.h>
#include <sys/backlight.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace midnight {
namespace {
constexpr int max_devices = 8;

std::string error(const char* action) {
  return std::string(action) + ": " + std::strerror(errno);
}
} // namespace

int next_backlight_level(int current, std::span<const std::uint32_t> levels, bool up, int step) {
  current = std::clamp(current, 0, 100);
  std::vector<int> valid;
  for (const auto level : levels) if (level >= 1 && level <= 100) valid.push_back(int(level));
  std::ranges::sort(valid);
  if (valid.size() >= 2) {
    if (up) {
      const auto next = std::ranges::upper_bound(valid, current);
      return next == valid.end() ? valid.back() : *next;
    }
    const auto next = std::ranges::lower_bound(valid, current);
    return next == valid.begin() ? valid.front() : *(next - 1);
  }
  return std::clamp(current + (up ? step : -step), 1, 100);
}

BacklightControl::BacklightControl(std::string device) : device_(std::move(device)) {
  // Isolated smoke tests name a private path so they never dim the real panel.
  if (device_.empty())
    if (const char* test = std::getenv("MIDNIGHT_WM_TEST_BACKLIGHT"); test && *test) device_ = test;
}

std::expected<int, std::string> BacklightControl::open_device(bool writable) const {
  const int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC;
  if (!device_.empty()) {
    const int fd = open(device_.c_str(), flags);
    if (fd < 0) return std::unexpected(error("Open backlight"));
    return fd;
  }
  // Prefer a display panel over a keyboard backlight; keep the first error,
  // which says whether the device is missing or not permitted.
  int first_error = ENOENT;
  for (int unit = 0; unit < max_devices; ++unit) {
    const auto path = "/dev/backlight/backlight" + std::to_string(unit);
    const int fd = open(path.c_str(), flags);
    if (fd < 0) {
      if (errno != ENOENT && first_error == ENOENT) first_error = errno;
      continue;
    }
    backlight_info info{};
    if (ioctl(fd, BACKLIGHTGETINFO, &info) == 0 && info.type == BACKLIGHT_TYPE_PANEL) return fd;
    close(fd);
  }
  errno = first_error;
  return std::unexpected(error("Open backlight"));
}

std::expected<int, std::string> BacklightControl::read() const {
  const auto fd = open_device(false);
  if (!fd) return std::unexpected(fd.error());
  backlight_props props{};
  const bool ok = ioctl(*fd, BACKLIGHTGETSTATUS, &props) == 0;
  const auto message = ok ? std::string() : error("Read backlight");
  close(*fd);
  if (!ok) return std::unexpected(message);
  return std::clamp(int(std::min<std::uint32_t>(props.brightness, 100)), 0, 100);
}

std::expected<int, std::string> BacklightControl::adjust(bool up) const {
  const auto fd = open_device(true);
  if (!fd) return std::unexpected(fd.error());
  backlight_props props{};
  if (ioctl(*fd, BACKLIGHTGETSTATUS, &props) < 0) {
    const auto message = error("Read backlight");
    close(*fd);
    return std::unexpected(message);
  }
  const auto count = std::min<std::uint32_t>(props.nlevels, BACKLIGHTMAXLEVELS);
  const int target = next_backlight_level(int(std::min<std::uint32_t>(props.brightness, 100)),
                                          std::span(props.levels, count), up);
  props.brightness = std::uint32_t(target);
  if (ioctl(*fd, BACKLIGHTUPDATESTATUS, &props) < 0) {
    const auto message = error("Set backlight");
    close(*fd);
    return std::unexpected(message);
  }
  close(*fd);
  return target;
}

} // namespace midnight
