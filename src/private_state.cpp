// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "private_state.hpp"
#include <glib.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace midnight {
namespace {
std::string system_error() { return std::strerror(errno); }
// Use directory-relative operations so replacing the pathname cannot redirect
// a read, write, or removal after the private directory has been opened.
std::expected<int, std::string> open_directory(const std::filesystem::path& path, bool create) {
  const auto directory = path.parent_path();
  if (create) {
    std::error_code error;
    std::filesystem::create_directories(directory.parent_path(), error);
    if (error) return std::unexpected(error.message());
    if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
      return std::unexpected(system_error());
  }
  const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::unexpected(system_error());
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_uid != getuid() || (st.st_mode & 0077)) {
    close(fd);
    return std::unexpected("Session directory must be owned by the user and private (0700)");
  }
  return fd;
}
struct Descriptor {
  int fd;
  ~Descriptor() { if (fd >= 0) close(fd); }
};
} // namespace

std::expected<std::optional<std::string>, std::string> read_private_state(
    const std::filesystem::path& path, std::size_t limit) {
  // Missing state is a fresh session; symlinks and insecure state are errors.
  struct stat st{};
  if (lstat(path.parent_path().c_str(), &st) != 0 && errno == ENOENT) return std::nullopt;
  auto directory = open_directory(path, false);
  if (!directory) return std::unexpected(directory.error());
  Descriptor dir{*directory};
  Descriptor file{openat(dir.fd, path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
  if (file.fd < 0) {
    if (errno == ENOENT) return std::nullopt;
    return std::unexpected(system_error());
  }
  if (fstat(file.fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
      (st.st_mode & 0077) || st.st_size < 0 || st.st_size > static_cast<off_t>(limit))
    return std::unexpected("Invalid or insecure session snapshot");
  std::string contents;
  char buffer[4096];
  for (;;) {
    const auto count = read(file.fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return std::unexpected(system_error());
    if (!count) break;
    contents.append(buffer, static_cast<std::size_t>(count));
    if (contents.size() > limit) return std::unexpected("Session snapshot is too large");
  }
  return contents;
}

std::expected<void, std::string> write_private_state(
    const std::filesystem::path& path, std::string_view contents) {
  auto directory = open_directory(path, true);
  if (!directory) return std::unexpected(directory.error());
  Descriptor dir{*directory};
  std::string temporary;
  int fd = -1;
  for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
    temporary = ".session-" + std::to_string(getpid()) + "-" + std::to_string(g_random_int());
    fd = openat(dir.fd, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0 && errno != EEXIST) return std::unexpected(system_error());
  }
  if (fd < 0) return std::unexpected("Cannot create temporary session snapshot");
  Descriptor file{fd};
  std::string error;
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto count = write(file.fd, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { error = system_error(); break; }
    offset += static_cast<std::size_t>(count);
  }
  if (error.empty() && fsync(file.fd) != 0) error = system_error();
  if (error.empty() && renameat(dir.fd, temporary.c_str(), dir.fd, path.filename().c_str()) != 0) error = system_error();
  if (!error.empty()) { unlinkat(dir.fd, temporary.c_str(), 0); return std::unexpected(error); }
  if (fsync(dir.fd) != 0) return std::unexpected(system_error());
  return {};
}

std::expected<void, std::string> clear_private_state(const std::filesystem::path& path) {
  struct stat st{};
  if (lstat(path.parent_path().c_str(), &st) != 0 && errno == ENOENT) return {};
  auto directory = open_directory(path, false);
  if (!directory) return std::unexpected(directory.error());
  Descriptor dir{*directory};
  if (unlinkat(dir.fd, path.filename().c_str(), 0) != 0 && errno != ENOENT) return std::unexpected(system_error());
  if (fsync(dir.fd) != 0) return std::unexpected(system_error());
  return {};
}
} // namespace midnight
