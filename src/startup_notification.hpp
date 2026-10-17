#pragma once

#include <gio/gio.h>
#include <xcb/xcb.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>

namespace midnight {

// Tracks GIO application launches using the X11 startup notification protocol.
class StartupNotification {
public:
  StartupNotification(xcb_connection_t* connection, xcb_screen_t* screen, int screen_number);
  StartupNotification(const StartupNotification&) = delete;
  StartupNotification& operator=(const StartupNotification&) = delete;

  bool launch(GAppInfo* app, GList* files, GError** error);
  bool launch_default_uri(const char* uri, GError** error);
  void record_user_time(xcb_timestamp_t time) { if (time) user_time_ = time; }
  void window_mapped(xcb_window_t window);
  bool handle(xcb_generic_event_t* event);
  void tick();
  [[nodiscard]] std::size_t pending_count() const { return pending_.size(); }
  void set_change_handler(std::function<void(std::size_t)> handler) { change_handler_ = std::move(handler); }

  // Used by the GAppLaunchContext subclass while GIO starts a process.
  [[nodiscard]] std::string begin(GAppInfo* app);
  // Takes a copy: callers may pass a key owned by pending_, which this erases.
  void finish(std::string id);

private:
  struct Pending {
    std::string wm_class;
    std::chrono::steady_clock::time_point started;
  };
  xcb_atom_t atom(const char* name);
  void broadcast(const std::string& message);
  void changed();

  xcb_connection_t* connection_{};
  xcb_screen_t* screen_{};
  int screen_number_{};
  xcb_timestamp_t user_time_{};
  unsigned long serial_{};
  std::map<std::string, Pending> pending_;
  std::map<xcb_window_t, std::string> fragments_;
  std::map<std::string, xcb_atom_t> atoms_;
  std::function<void(std::size_t)> change_handler_;
};

} // namespace midnight
