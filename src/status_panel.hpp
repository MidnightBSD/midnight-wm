#pragma once

#include <xcb/xcb.h>

#include <string>
#include <vector>

namespace midnight {
class Shell;
class StatusNotifier;
class X11SystemTray;
class DbusMenuPopup;

// X11 presentation for StatusNotifier items; the D-Bus protocol lives in StatusNotifier.
class StatusNotifierPanel {
public:
  StatusNotifierPanel(xcb_connection_t* connection, xcb_screen_t* screen, Shell& shell,
                      X11SystemTray& tray, StatusNotifier& notifier, DbusMenuPopup& menu_popup);
  ~StatusNotifierPanel();
  StatusNotifierPanel(const StatusNotifierPanel&) = delete;
  StatusNotifierPanel& operator=(const StatusNotifierPanel&) = delete;

  [[nodiscard]] bool initialize();
  [[nodiscard]] bool handle(xcb_generic_event_t* event);
  void refresh();
  void tick();

private:
  void paint();

  xcb_connection_t* connection_{};
  xcb_screen_t* screen_{};
  Shell& shell_;
  X11SystemTray& tray_;
  StatusNotifier& notifier_;
  DbusMenuPopup& menu_popup_;
  xcb_window_t window_{};
  std::vector<std::string> visible_keys_;
  std::string active_menu_key_;
  int x_{};
  int width_{};
};
} // namespace midnight
