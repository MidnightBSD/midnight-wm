#pragma once

#include <xcb/xcb.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace midnight {
class Shell;

// Freedesktop system tray selection owner and XEmbed container for X11 icons.
class X11SystemTray {
public:
  X11SystemTray(xcb_connection_t* connection, xcb_screen_t* screen, int screen_number, Shell& shell);
  ~X11SystemTray();
  X11SystemTray(const X11SystemTray&) = delete;
  X11SystemTray& operator=(const X11SystemTray&) = delete;

  [[nodiscard]] bool initialize();
  [[nodiscard]] bool handle(xcb_generic_event_t* event);
  void tick();
  void refresh();
  void set_external_width(int width);
  [[nodiscard]] int external_x() const;
  [[nodiscard]] int external_width() const;

private:
  struct Icon { bool mapped{true}; };
  xcb_atom_t intern(const std::string& name) const;
  void dock(xcb_window_t window);
  void remove(xcb_window_t window, bool destroyed);
  void refresh(xcb_window_t window);
  void layout();
  void paint();
  void send_xembed(xcb_window_t window, std::uint32_t opcode, std::uint32_t detail = 0);
  void release();

  xcb_connection_t* connection_{};
  xcb_screen_t* screen_{};
  int screen_number_{};
  Shell& shell_;
  xcb_window_t owner_{};
  xcb_window_t host_{};
  xcb_atom_t selection_{};
  xcb_atom_t opcode_{};
  xcb_atom_t xembed_{};
  xcb_atom_t xembed_info_{};
  std::map<xcb_window_t, Icon> icons_;
  std::vector<xcb_window_t> order_;
  int host_x_{};
  int host_width_{};
  int external_requested_width_{};
  int layout_x_{};
  int layout_width_{};
  int layout_height_{};
  int layout_icon_size_{};
};
} // namespace midnight
