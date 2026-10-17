#pragma once

#include <glib.h>

#include <string>

namespace midnight {

// Initializes optional password storage and SSH authentication before XDG
// autostart applications are launched. Owns only an ssh-agent it starts.
class KeyringSession {
 public:
  KeyringSession() = default;
  KeyringSession(const KeyringSession&) = delete;
  KeyringSession& operator=(const KeyringSession&) = delete;
  ~KeyringSession();

  void initialize();

 private:
  GPid ssh_agent_pid_{};
  std::string ssh_agent_directory_;
};

} // namespace midnight
