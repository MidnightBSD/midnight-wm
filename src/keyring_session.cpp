#include "keyring_session.hpp"

#include <gio/gio.h>
#include <glib.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace midnight {
namespace {
using GString = std::unique_ptr<char, decltype(&g_free)>;

bool working_ssh_socket(const char* path) {
  if (!path || !*path || std::strlen(path) >= sizeof(sockaddr_un::sun_path)) return false;
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path, std::strlen(path) + 1);
  const bool reachable = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  close(fd);
  return reachable;
}

bool bus_name_has_owner(GDBusConnection* bus, const char* name) {
  GError* error = nullptr;
  auto* reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "NameHasOwner", g_variant_new("(s)", name), G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
  if (error) g_error_free(error);
  if (!reply) return false;
  gboolean owned = FALSE;
  g_variant_get(reply, "(b)", &owned);
  g_variant_unref(reply);
  return owned;
}

bool activate_secret_service(GDBusConnection* bus) {
  if (bus_name_has_owner(bus, "org.freedesktop.secrets")) return true;
  // D-Bus activation follows the installed service file, including a user
  // override. It may select GNOME, KWallet, KeePassXC, or another provider.
  GError* error = nullptr;
  auto* reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "StartServiceByName", g_variant_new("(su)", "org.freedesktop.secrets", 0u),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error);
  if (error) g_error_free(error);
  if (reply) g_variant_unref(reply);
  return bus_name_has_owner(bus, "org.freedesktop.secrets");
}

std::string environment_assignment(std::string_view output, std::string_view key) {
  for (size_t offset = 0; offset < output.size();) {
    const size_t end = output.find('\n', offset);
    auto line = output.substr(offset, end == std::string_view::npos ? end : end - offset);
    if (line.starts_with(key) && line.size() > key.size() && line[key.size()] == '=') {
      line.remove_prefix(key.size() + 1);
      line = line.substr(0, line.find(';'));
      if (line.size() >= 2 && (line.front() == '\'' || line.front() == '"') &&
          line.back() == line.front()) line = line.substr(1, line.size() - 2);
      return std::string(line);
    }
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return {};
}

bool start_gnome_keyring(bool secrets, bool ssh) {
  GString program(g_find_program_in_path("gnome-keyring-daemon"), g_free);
  if (!program) return false;
  std::string components = "--components=";
  if (secrets) components += "secrets";
  if (ssh) {
    if (secrets) components += ',';
    components += "ssh";
  }
  char start[] = "--start";
  char* argv[]{program.get(), start, components.data(), nullptr};
  char* output = nullptr;
  char* diagnostic = nullptr;
  int status = 0;
  GError* error = nullptr;
  const gboolean launched = g_spawn_sync(nullptr, argv, nullptr, G_SPAWN_DEFAULT,
                                         nullptr, nullptr, &output, &diagnostic,
                                         &status, &error);
  GString stdout_text(output, g_free);
  GString stderr_text(diagnostic, g_free);
  if (!launched || !g_spawn_check_wait_status(status, &error)) {
    std::cerr << "GNOME Keyring startup: " << (error ? error->message : "failed") << '\n';
    if (error) g_error_free(error);
    return false;
  }
  const auto socket = environment_assignment(stdout_text ? stdout_text.get() : "", "SSH_AUTH_SOCK");
  if (ssh && working_ssh_socket(socket.c_str())) g_setenv("SSH_AUTH_SOCK", socket.c_str(), TRUE);
  return true;
}
} // namespace

KeyringSession::~KeyringSession() {
  if (ssh_agent_pid_) {
    kill(ssh_agent_pid_, SIGTERM);
    int status = 0;
    while (waitpid(ssh_agent_pid_, &status, 0) < 0 && errno == EINTR) {}
    g_spawn_close_pid(ssh_agent_pid_);
  }
  if (!ssh_agent_directory_.empty()) {
    const auto socket = ssh_agent_directory_ + "/agent.sock";
    unlink(socket.c_str());
    rmdir(ssh_agent_directory_.c_str());
  }
}

void KeyringSession::initialize() {
  GError* error = nullptr;
  auto* bus = g_getenv("DBUS_SESSION_BUS_ADDRESS")
                  ? g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error)
                  : nullptr;
  g_clear_error(&error);
  bool secret_service = false;
  if (bus) {
    secret_service = activate_secret_service(bus);
    g_object_unref(bus);
  }

  bool ssh_ready = working_ssh_socket(g_getenv("SSH_AUTH_SOCK"));
  if (!ssh_ready && g_getenv("SSH_AUTH_SOCK")) g_unsetenv("SSH_AUTH_SOCK");

  // The D-Bus service file may start only the secrets component. Complete a
  // GNOME PAM login and obtain its SSH socket before launching applications.
  if (!secret_service || !ssh_ready) {
    (void)start_gnome_keyring(!secret_service, !ssh_ready);
    ssh_ready = working_ssh_socket(g_getenv("SSH_AUTH_SOCK"));
  }
  if (ssh_ready) return;

  GString agent(g_find_program_in_path("ssh-agent"), g_free);
  if (!agent) return;
  GString temporary(g_dir_make_tmp("midnight-wm-agent-XXXXXX", &error), g_free);
  if (!temporary) {
    std::cerr << "SSH agent directory: " << (error ? error->message : "failed") << '\n';
    if (error) g_error_free(error);
    return;
  }
  ssh_agent_directory_ = temporary.get();
  const auto path = ssh_agent_directory_ + "/agent.sock";
  if (path.size() >= sizeof(sockaddr_un::sun_path)) return;
  char foreground[] = "-D";
  char socket_arg[] = "-a";
  char* argv[]{agent.get(), foreground, socket_arg, const_cast<char*>(path.c_str()), nullptr};
  if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_DO_NOT_REAP_CHILD,
                     nullptr, nullptr, &ssh_agent_pid_, &error)) {
    std::cerr << "SSH agent startup: " << (error ? error->message : "failed") << '\n';
    if (error) g_error_free(error);
    return;
  }
  for (int attempt = 0; attempt < 80; ++attempt) {
    if (working_ssh_socket(path.c_str())) {
      g_setenv("SSH_AUTH_SOCK", path.c_str(), TRUE);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  std::cerr << "SSH agent did not create its socket\n";
}

} // namespace midnight
