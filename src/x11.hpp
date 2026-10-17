#pragma once

#include <xcb/xcb.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace midnight {
class Shell;

class X11WindowManager {
public:
  X11WindowManager(xcb_connection_t* connection, xcb_screen_t* screen, Shell& shell);
  ~X11WindowManager();
  X11WindowManager(const X11WindowManager&) = delete;
  X11WindowManager& operator=(const X11WindowManager&) = delete;

  bool initialize();
  void handle(xcb_generic_event_t* event);
  void update_screen_size(std::uint16_t width, std::uint16_t height);

private:
  struct Frame {
    xcb_window_t client{};
    xcb_window_t frame{};
    int x{};
    int y{};
    std::uint16_t width{};
    std::uint16_t height{};
    int old_x{};
    int old_y{};
    std::uint16_t old_width{};
    std::uint16_t old_height{};
    int ignore_unmap{};
    bool maximized{};
    bool fullscreen{};
    bool mapped{};
    std::string title;
  };
  struct Monitor { int x{}, y{}, width{}, height{}; };

  enum class DragMode { none, move, resize };
  xcb_atom_t atom(const std::string& name);
  void set_root_properties();
  void update_geometry_properties();
  void update_client_list();
  void update_tasks();
  void scan_existing();
  void manage(xcb_window_t window);
  void unmanage(xcb_window_t client, bool destroyed);
  void map(xcb_window_t client);
  void focus(xcb_window_t client);
  void close(xcb_window_t client);
  void toggle_maximize(xcb_window_t client);
  void set_fullscreen(xcb_window_t client, bool enabled);
  void minimize(xcb_window_t client);
  void restore(xcb_window_t client);
  void resize_frame(Frame& frame);
  void draw_frame(Frame& frame);
  void update_title(Frame& frame);
  void install_key_grabs();
  void cycle_focus();
  xcb_window_t client_for(xcb_window_t window) const;
  xcb_visualtype_t* root_visual() const;
  Monitor monitor_for(const Frame& frame) const;

  xcb_connection_t* connection_{};
  xcb_screen_t* screen_{};
  Shell& shell_;
  std::map<xcb_window_t, Frame> frames_;
  std::map<xcb_window_t, xcb_window_t> frame_to_client_;
  std::map<std::string, xcb_atom_t> atoms_;
  xcb_window_t support_window_{};
  xcb_window_t active_{};
  std::uint16_t screen_width_{};
  std::uint16_t screen_height_{};
  DragMode drag_mode_{DragMode::none};
  xcb_window_t drag_client_{};
  int drag_root_x_{};
  int drag_root_y_{};
  int drag_x_{};
  int drag_y_{};
  std::uint16_t drag_width_{};
  std::uint16_t drag_height_{};
};
} // namespace midnight
