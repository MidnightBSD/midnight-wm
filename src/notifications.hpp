#pragma once

#include <memory>
#include <xcb/xcb.h>

namespace midnight {

class Shell;

// Optional session notification service. GLib callbacks run on the main loop's
// default context; X11 events and expiration are driven by main.cpp.
class Notifications {
 public:
  Notifications(xcb_connection_t* connection, xcb_screen_t* screen, Shell& shell);
  ~Notifications();
  Notifications(const Notifications&) = delete;
  Notifications& operator=(const Notifications&) = delete;

  bool initialize();
  bool handle(xcb_generic_event_t* event);
  void tick();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace midnight
