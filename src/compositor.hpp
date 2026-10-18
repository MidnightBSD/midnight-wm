#pragma once

#include <xcb/xcb.h>
#include <chrono>
#include <memory>

namespace midnight {

// Session-owned, foreground Picom. Never replaces an existing compositor.
class Compositor {
 public:
  Compositor(xcb_connection_t* connection, int screen_number);
  ~Compositor();
  Compositor(const Compositor&) = delete;
  Compositor& operator=(const Compositor&) = delete;
  void set_enabled(bool enabled);
  void tick();

 private:
  struct Child;
  xcb_connection_t* connection_;
  xcb_atom_t selection_{};
  std::shared_ptr<Child> child_;
  bool enabled_{};
  bool attempted_{};
  std::chrono::steady_clock::time_point last_check_{};
  std::chrono::steady_clock::time_point stop_deadline_{};
  void stop();
};

} // namespace midnight
