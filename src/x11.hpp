#pragma once

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include <cstdint>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace midnight {
class Shell;
class StartupNotification;

class X11WindowManager {
public:
  X11WindowManager(xcb_connection_t* connection, xcb_screen_t* screen, Shell& shell);
  ~X11WindowManager();
  X11WindowManager(const X11WindowManager&) = delete;
  X11WindowManager& operator=(const X11WindowManager&) = delete;

  bool initialize();
  void handle(xcb_generic_event_t* event);
  void update_screen_size(std::uint16_t width, std::uint16_t height);
  void set_startup_notification(StartupNotification* startup) { startup_ = startup; }

private:
  enum class WindowRole { normal, dialog, utility, splash };
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
    bool maximized_horz{};
    bool maximized_vert{};
    bool fullscreen{};
    bool mapped{};
    bool minimized{};
    bool accepts_input{true};
    bool takes_focus{};
    bool skip_taskbar{};
    bool modal{};
    WindowRole role{WindowRole::normal};
    xcb_window_t transient_for{};
    xcb_size_hints_t size_hints{};
    std::string title;
  };
  struct Monitor { int x{}, y{}, width{}, height{}; };
  struct Dock {
    std::array<std::uint32_t, 12> strut{};
    bool mapped{};
  };

  enum class DragMode { none, move, resize, keyboard_move, keyboard_resize };
  xcb_atom_t atom(const std::string& name);
  void set_root_properties();
  void update_geometry_properties();
  void refresh_dock(xcb_window_t window);
  void update_maximized_geometry(Frame& frame);
  Monitor workarea_for(const Monitor& monitor) const;
  void update_client_list();
  void update_stacking();
  void restack_layers();
  void update_tasks();
  void scan_existing();
  void manage(xcb_window_t window);
  void unmanage(xcb_window_t client, bool destroyed);
  void map(xcb_window_t client);
  void focus(xcb_window_t client, xcb_timestamp_t time = XCB_CURRENT_TIME);
  void update_icccm_hints(Frame& frame);
  void constrain_size(const Frame& frame, std::uint16_t& width, std::uint16_t& height) const;
  void update_window_state(const Frame& frame);
  bool decorated(const Frame& frame) const;
  void set_maximized(xcb_window_t client, bool horz, bool vert);
  void close(xcb_window_t client);
  void toggle_maximize(xcb_window_t client);
  void set_fullscreen(xcb_window_t client, bool enabled);
  void minimize(xcb_window_t client);
  void restore(xcb_window_t client);
  void resize_frame(Frame& frame);
  void configure_frame(Frame& frame, std::uint16_t mask, std::int32_t x, std::int32_t y,
                       std::uint32_t width, std::uint32_t height, std::uint8_t gravity = 0);
  void send_configure_notify(const Frame& frame);
  void begin_drag(Frame& frame, DragMode mode, int root_x, int root_y,
                  std::uint8_t direction, std::uint8_t button);
  void end_drag(bool cancel);
  void draw_frame(Frame& frame);
  void update_title(Frame& frame);
  void install_key_grabs();
  bool install_root_cursor();
  void cycle_focus();
  [[nodiscard]] int border_pixels() const;
  [[nodiscard]] int title_height_pixels() const;
  xcb_window_t client_for(xcb_window_t window) const;
  xcb_visualtype_t* root_visual() const;
  Monitor monitor_for(const Frame& frame) const;

  xcb_connection_t* connection_{};
  xcb_screen_t* screen_{};
  Shell& shell_;
  StartupNotification* startup_{};
  std::map<xcb_window_t, Frame> frames_;
  std::vector<xcb_window_t> client_order_;
  std::map<xcb_window_t, Dock> docks_;
  std::map<xcb_window_t, xcb_window_t> frame_to_client_;
  std::map<std::string, xcb_atom_t> atoms_;
  xcb_window_t support_window_{};
  xcb_cursor_t root_cursor_{};
  xcb_window_t active_{};
  xcb_timestamp_t last_event_time_{};
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
  std::uint8_t drag_direction_{};
  std::uint8_t drag_button_{};
};
} // namespace midnight
