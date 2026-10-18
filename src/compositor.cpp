// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "compositor.hpp"

#include <gio/gio.h>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace midnight {

struct Compositor::Child {
  GSubprocess* process{};
  bool exited{};
  bool stopping{};
  ~Child() { if (process) g_object_unref(process); }
};

Compositor::Compositor(xcb_connection_t* connection, int screen_number)
    : connection_(connection) {
  const auto name = "_NET_WM_CM_S" + std::to_string(screen_number);
  auto* reply = xcb_intern_atom_reply(connection_,
      xcb_intern_atom(connection_, false, name.size(), name.c_str()), nullptr);
  if (reply) { selection_ = reply->atom; std::free(reply); }
}

Compositor::~Compositor() {
  // GSubprocess tracks/reaps its own child; callbacks retain no pointer to us.
  if (child_ && !child_->exited) g_subprocess_force_exit(child_->process);
}

void Compositor::stop() {
  if (!child_ || child_->exited || child_->stopping) return;
  child_->stopping = true;
  stop_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  g_subprocess_send_signal(child_->process, SIGTERM);
}

void Compositor::set_enabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  attempted_ = false;
  last_check_ = {};
  if (!enabled) stop();
}

void Compositor::tick() {
  const auto now = std::chrono::steady_clock::now();
  if (child_) {
    if (child_->exited) {
      if (!child_->stopping) std::cerr << "Workspace compositor exited; toggle compositing to retry\n";
      child_.reset();
    } else {
      if (child_->stopping && now >= stop_deadline_)
        g_subprocess_force_exit(child_->process);
      return;
    }
  }
  if (!enabled_ || attempted_ || !selection_ || xcb_connection_has_error(connection_)) return;
  if (now - last_check_ < std::chrono::seconds(1)) return;
  last_check_ = now;
  auto* owner = xcb_get_selection_owner_reply(connection_,
      xcb_get_selection_owner(connection_, selection_), nullptr);
  if (!owner) return;
  const bool occupied = owner->owner != XCB_NONE;
  std::free(owner);
  if (occupied) return;

  attempted_ = true;
  for (const auto* name : {"Composite", "DAMAGE", "RENDER", "XFIXES"}) {
    const std::string extension(name);
    auto* reply = xcb_query_extension_reply(connection_,
        xcb_query_extension(connection_, extension.size(), extension.c_str()), nullptr);
    const bool available = reply && reply->present;
    std::free(reply);
    if (!available) {
      std::cerr << "Workspace compositing unavailable: missing " << name << " extension\n";
      return;
    }
  }
  char* program = g_find_program_in_path("picom");
  if (!program) {
    std::cerr << "Workspace compositing unavailable: install picom for window shadows\n";
    return;
  }
  // Fixed arguments, no shell, and no unrelated global/user Picom configuration.
  // Picom keeps the compositor selection and refuses a competing owner itself.
  const char* argv[]{program, "--config", "/dev/null", "--backend", "xrender",
      "--shadow", "--shadow-radius", "12", "--shadow-opacity", "0.35",
      "--shadow-offset-x", "-6", "--shadow-offset-y", "-4",
      "--shadow-exclude", "window_type = 'dock' || window_type = 'desktop' || fullscreen",
      nullptr};
  GError* error = nullptr;
  auto* process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE, &error);
  g_free(program);
  if (!process) {
    std::cerr << "Workspace compositor: " << (error ? error->message : "launch failed") << '\n';
    if (error) g_error_free(error);
    return;
  }
  child_ = std::make_shared<Child>();
  child_->process = process;
  auto* pending = new std::shared_ptr<Child>(child_);
  g_subprocess_wait_async(process, nullptr, [](GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<std::shared_ptr<Child>> child(static_cast<std::shared_ptr<Child>*>(data));
    (void)g_subprocess_wait_finish(G_SUBPROCESS(source), result, nullptr);
    (*child)->exited = true;
  }, pending);
}

} // namespace midnight
