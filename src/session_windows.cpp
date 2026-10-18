// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "session_windows.hpp"
#include "private_state.hpp"
#include <glib.h>
#include <array>
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <memory>
#include <set>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace midnight {
namespace {
constexpr auto limit = 1024 * 1024;
constexpr auto type = "a(ayayiiiiubbbb)";
using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
bool valid(const std::vector<ApplicationWindow>& windows) {
  if (windows.size() > 512) return false;
  std::set<std::pair<std::string, std::string>> identities;
  for (const auto& window : windows)
    if (!validate_application_window(window) || !identities.emplace(window.client_id, window.role).second) return false;
  return true;
}
bool send_byte(int fd, char byte) {
  return send(fd, &byte, 1, MSG_DONTWAIT | MSG_NOSIGNAL) == 1;
}
void clear_environment() {
  g_unsetenv("MIDNIGHT_WM_WINDOW_FD");
  g_unsetenv("MIDNIGHT_WM_WINDOW_REPORT");
}
}
std::expected<void, std::string> write_window_report(const std::filesystem::path& path, const WindowReport& windows) {
  if (!windows) return std::unexpected(windows.error());
  if (!valid(*windows)) return std::unexpected("Invalid window report");
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE(type));
  for (const auto& w : *windows) {
    g_variant_builder_add(&builder, "(@ay@ayiiiiubbbb)",
      g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, w.client_id.data(), w.client_id.size(), 1),
      g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, w.role.data(), w.role.size(), 1),
      w.geometry.x, w.geometry.y, w.geometry.width, w.geometry.height, w.workspace,
      w.maximized_horz, w.maximized_vert, w.fullscreen, w.shaded);
  }
  Variant value(g_variant_ref_sink(g_variant_builder_end(&builder)), g_variant_unref);
  if (g_variant_get_size(value.get()) > limit - 4) return std::unexpected("Window report too large");
  std::string contents = "MWP1";
  if (g_variant_get_size(value.get()))
    contents.append(static_cast<const char*>(g_variant_get_data(value.get())), g_variant_get_size(value.get()));
  return write_private_state(path, contents);
}
WindowReport read_window_report(const std::filesystem::path& path) {
  auto contents = read_private_state(path, limit);
  if (!contents) return std::unexpected(contents.error());
  if (!*contents || !(**contents).starts_with("MWP1")) return std::unexpected("Missing or invalid window report");
  GBytes* bytes = g_bytes_new((**contents).data() + 4, (**contents).size() - 4);
  Variant value(g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE(type), bytes, false)), g_variant_unref);
  g_bytes_unref(bytes);
  if (!g_variant_is_normal_form(value.get()) || g_variant_n_children(value.get()) > 512)
    return std::unexpected("Malformed window report");
  std::vector<ApplicationWindow> windows;
  for (gsize i = 0; i < g_variant_n_children(value.get()); ++i) {
    Variant child(g_variant_get_child_value(value.get(), i), g_variant_unref);
    GVariant *id{}, *role{};
    ApplicationWindow w;
    gboolean horz{}, vert{}, full{}, shade{};
    g_variant_get(child.get(), "(@ay@ayiiiiubbbb)", &id, &role,
      &w.geometry.x, &w.geometry.y, &w.geometry.width, &w.geometry.height, &w.workspace,
      &horz, &vert, &full, &shade);
    gsize length{};
    const auto* data = static_cast<const char*>(g_variant_get_fixed_array(id, &length, 1));
    if (length) w.client_id.assign(data, length);
    data = static_cast<const char*>(g_variant_get_fixed_array(role, &length, 1));
    if (length) w.role.assign(data, length);
    g_variant_unref(id); g_variant_unref(role);
    w.maximized_horz = horz; w.maximized_vert = vert; w.fullscreen = full; w.shaded = shade;
    windows.push_back(std::move(w));
  }
  if (!valid(windows)) return std::unexpected("Invalid window report values");
  return windows;
}
SessionWindowChannel::~SessionWindowChannel() {
  if (parent_ >= 0) close(parent_);
  if (child_ >= 0) close(child_);
  if (!path_.empty()) unlink(path_.c_str());
}
std::expected<void, std::string> SessionWindowChannel::initialize(const std::filesystem::path& directory) {
  std::array<int, 2> sockets;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0) return std::unexpected(std::strerror(errno));
  parent_ = sockets[0]; child_ = sockets[1]; path_ = directory / "windows.bin";
  for (int fd : sockets)
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
      return std::unexpected(std::strerror(errno));
  g_setenv("MIDNIGHT_WM_WINDOW_FD", std::to_string(child_).c_str(), true);
  g_setenv("MIDNIGHT_WM_WINDOW_REPORT", path_.c_str(), true);
  return {};
}
int SessionWindowChannel::release_child_fd() { const int fd = child_; child_ = -1; return fd; }
void SessionWindowChannel::tick() {
  if (waiting_ && std::chrono::steady_clock::now() >= deadline_) { waiting_ = false; fresh_ = false; failed_ = true; }
  for (int i = 0; parent_ >= 0 && i < 16; ++i) {
    char byte{};
    const auto count = recv(parent_, &byte, 1, MSG_DONTWAIT);
    if (count == 0) disconnected_ = true;
    if (count <= 0) break;
    if (byte == 'L') { logout_requested_ = true; logout_aborted_ = false; }
    else if (byte == 'X') logout_aborted_ = true;
    else if (byte == 'R') ready_ = true;
    else if ((byte == 'D' && waiting_) || byte == 'F') {
      fresh_ = true; waiting_ = false; finished_ = byte == 'F';
    } else if (byte == 'E') { fresh_ = false; waiting_ = false; }
  }
}
bool SessionWindowChannel::take_logout_request() {
  const bool requested = logout_requested_;
  logout_requested_ = false;
  return requested;
}
void SessionWindowChannel::logout_reply(bool approved) {
  (void)send_byte(parent_, approved ? 'A' : 'C');
}
void SessionWindowChannel::request() {
  tick();
  if (!ready_ || finished_ || failed_) return;
  fresh_ = false;
  waiting_ = send_byte(parent_, 'Q');
  deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
}
WindowReport SessionWindowChannel::result() const {
  if (!fresh_) return std::unexpected("Window capture unavailable or timed out");
  return read_window_report(path_);
}
SessionWindowReporter::SessionWindowReporter() {
  const char* number = g_getenv("MIDNIGHT_WM_WINDOW_FD");
  const std::string raw = number ? number : "";
  const char* report = g_getenv("MIDNIGHT_WM_WINDOW_REPORT");
  const std::string path = report ? report : "";
  clear_environment();
  int fd{};
  const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), fd);
  if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() || fd < 3 ||
      path.empty() || !std::filesystem::path(path).is_absolute()) return;
  sockaddr_un peer{};
  socklen_t size = sizeof(peer);
  int kind{}; socklen_t kind_size = sizeof(kind);
  if (getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &size) != 0 || peer.sun_family != AF_UNIX ||
      size > sizeof(peer) || std::ranges::any_of(peer.sun_path, [](char byte) { return byte != 0; }) ||
      getsockopt(fd, SOL_SOCKET, SO_TYPE, &kind, &kind_size) != 0 || kind != SOCK_STREAM) return;
  fd_ = fd; path_ = path;
  if (fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd_, F_SETFL, O_NONBLOCK) < 0) { close(fd_); fd_ = -1; }
}
SessionWindowReporter::~SessionWindowReporter() { if (fd_ >= 0) close(fd_); }
void SessionWindowReporter::ready() { if (fd_ >= 0) (void)send_byte(fd_, 'R'); }
bool SessionWindowReporter::request_logout() {
  if (fd_ < 0) return false;
  if (logout_failed_) { logout_result_ = false; return true; }
  if (logout_pending_) return true;
  if (!send_byte(fd_, 'L')) { logout_result_ = false; return true; }
  logout_pending_ = true;
  logout_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(75);
  return true;
}
std::optional<bool> SessionWindowReporter::logout_result() {
  const auto result = logout_result_;
  logout_result_.reset();
  return result;
}
void SessionWindowReporter::publish(const WindowReport& windows, char success) {
  if (fd_ < 0) return;
  const auto saved = write_window_report(path_, windows);
  (void)send_byte(fd_, saved ? success : 'E');
}
void SessionWindowReporter::tick(const std::function<WindowReport()>& capture) {
  if (logout_pending_ && std::chrono::steady_clock::now() >= logout_deadline_) {
    (void)send_byte(fd_, 'X');
    logout_pending_ = false;
    logout_failed_ = true; // Never accept an approval queued after the deadline.
    logout_result_ = false;
  }
  bool requested{};
  for (int i = 0; fd_ >= 0 && i < 16; ++i) {
    char byte{};
    const auto count = recv(fd_, &byte, 1, MSG_DONTWAIT);
    if (count == 0 && logout_pending_) {
      logout_pending_ = false; logout_failed_ = true; logout_result_ = false;
    }
    if (count != 1) break;
    if ((byte == 'A' || byte == 'C') && logout_pending_) {
      logout_pending_ = false; logout_result_ = byte == 'A';
    }
    if (byte == 'Q') requested = true;
  }
  if (requested) publish(capture(), 'D');
}
void SessionWindowReporter::finish(const WindowReport& windows) { publish(windows, 'F'); }
} // namespace midnight
