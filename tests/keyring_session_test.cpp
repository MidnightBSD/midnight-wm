#include "keyring_session.hpp"

#include <glib.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
void require(bool condition) {
  if (!condition) std::abort();
}

bool connects(const std::string& path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const bool connected = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  close(fd);
  return connected;
}
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  // Keep tests off the user's bus and desktop. A concrete invalid address also
  // prevents GIO from autolaunching a bus while exercising the fallback path.
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/midnight-wm-test-no-bus", TRUE);
  std::unique_ptr<char, decltype(&g_free)> directory(
      g_dir_make_tmp("midnight-wm-keyring-test-XXXXXX", nullptr), g_free);
  require(bool(directory));
  const std::filesystem::path root(directory.get());
  const auto socket_path = (root / "existing.sock").string();
  if (std::string(argv[1]) == "inherited") {
    g_setenv("PATH", root.c_str(), TRUE);
    const int server = socket(AF_UNIX, SOCK_STREAM, 0);
    require(server >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    require(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    require(listen(server, 1) == 0);
    g_setenv("SSH_AUTH_SOCK", socket_path.c_str(), TRUE);
    {
      midnight::KeyringSession session;
      session.initialize();
      require(std::string(g_getenv("SSH_AUTH_SOCK")) == socket_path);
    }
    require(std::filesystem::exists(socket_path));
    close(server);
    std::filesystem::remove(socket_path);
  } else if (std::string(argv[1]) == "fallback") {
    std::unique_ptr<char, decltype(&g_free)> agent(g_find_program_in_path("ssh-agent"), g_free);
    if (!agent) return 77;
    const auto link = root / "ssh-agent";
    std::filesystem::create_symlink(agent.get(), link);
    g_setenv("PATH", root.c_str(), TRUE);
    g_unsetenv("SSH_AUTH_SOCK");
    std::string spawned_socket;
    {
      midnight::KeyringSession session;
      session.initialize();
      const char* path = g_getenv("SSH_AUTH_SOCK");
      require(path && *path);
      spawned_socket = path;
      require(connects(spawned_socket));
    }
    require(!connects(spawned_socket));
  } else return 2;
  std::filesystem::remove_all(root);
}
