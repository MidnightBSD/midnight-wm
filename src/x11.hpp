#pragma once

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include "display_monitors.hpp"

#include <cstdint>
#include <chrono>
#include <array>
#include <map>
#include <optional>
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
  void observe_event(const xcb_generic_event_t* event);
  void tick();
  void set_current_desktop(std::uint32_t desktop);
  void set_showing_desktop(bool showing);
  [[nodiscard]] std::uint32_t current_desktop() const { return current_desktop_; }
  [[nodiscard]] bool showing_desktop() const { return showing_desktop_; }
  [[nodiscard]] bool resize_pending() const;
  void update_screen_size(std::uint16_t width, std::uint16_t height);
  void refresh_theme();
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
    std::uint32_t desktop{};
    bool visible{};
    bool supports_ping{};
    bool unresponsive{};
    xcb_timestamp_t ping_time{};
    xcb_timestamp_t previous_ping_time{};
    std::chrono::steady_clock::time_point ping_deadline{};
    std::chrono::steady_clock::time_point next_ping{};
    std::uint32_t sync_counter{};
    std::uint64_t sync_value{};
    unsigned sync_query{};
    std::uint16_t sync_width{};
    std::uint16_t sync_height{};
    bool sync_waiting{};
    bool sync_deferred{};
    std::chrono::steady_clock::time_point sync_deadline{};
    bool accepts_input{true};
    bool takes_focus{};
    bool skip_taskbar{};
    bool modal{};
    bool above{};
    bool below{};
    bool skip_pager{};
    bool attention{};
    bool urgent{};
    xcb_window_t user_time_window{};
    std::optional<xcb_timestamp_t> user_time;
    WindowRole role{WindowRole::normal};
    xcb_window_t transient_for{};
    xcb_atom_t monitor_name{};
    xcb_size_hints_t size_hints{};
    std::string title;
  };
  using Monitor = DisplayMonitor;
  struct Dock {
    std::array<std::uint32_t, 12> strut{};
    bool mapped{};
  };

  enum class DragMode { none, move, resize, keyboard_move, keyboard_resize };
  xcb_atom_t atom(const std::string& name);
  void set_root_properties();
  void refresh_visibility();
  void set_window_desktop(Frame& frame, std::uint32_t desktop);
  void initialize_sync(Frame& frame);
  void send_protocol(xcb_window_t client, xcb_atom_t protocol, std::uint32_t value1,
                     std::uint32_t value2 = 0, std::uint32_t value3 = 0);
  void update_geometry_properties();
  void refresh_dock(xcb_window_t window);
  void update_maximized_geometry(Frame& frame);
  Monitor workarea_for(const Monitor& monitor) const;
  void update_client_list();
  void update_stacking();
  void restack_layers(bool force = true);
  void update_tasks();
  void scan_existing();
  void manage(xcb_window_t window);
  void unmanage(xcb_window_t client, bool destroyed);
  void map(xcb_window_t client, bool activate = true);
  void focus(xcb_window_t client, xcb_timestamp_t time = XCB_CURRENT_TIME);
  void update_icccm_hints(Frame& frame);
  void constrain_size(const Frame& frame, std::uint16_t& width, std::uint16_t& height) const;
  void fit_dialog(Frame& frame);
  void update_window_state(const Frame& frame);
  void update_allowed_actions(const Frame& frame);
  void set_attention(Frame& frame, bool attention);
  void clear_attention(Frame& frame);
  void refresh_user_time(Frame& frame);
  void release_user_time_window(xcb_window_t window);
  std::optional<std::uint32_t> cardinal(xcb_window_t window, xcb_atom_t name, xcb_atom_t type);
  bool can_activate(const Frame& frame, std::optional<xcb_timestamp_t> timestamp) const;
  void clear_focus();
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
  void refresh_display_layout(std::uint16_t width, std::uint16_t height);

  xcb_connection_t* connection_{};
  xcb_screen_t* screen_{};
  Shell& shell_;
  StartupNotification* startup_{};
  std::map<xcb_window_t, Frame> frames_;
  std::vector<xcb_window_t> client_order_;
  std::vector<xcb_window_t> last_root_order_;
  std::map<xcb_window_t, Dock> docks_;
  std::map<xcb_window_t, xcb_window_t> frame_to_client_;
  std::map<std::string, xcb_atom_t> atoms_;
  xcb_window_t support_window_{};
  xcb_cursor_t root_cursor_{};
  xcb_window_t active_{};
  static constexpr std::uint32_t desktop_count_ = 4;
  static constexpr std::uint32_t all_desktops_ = 0xffffffffU;
  std::uint32_t current_desktop_{};
  bool showing_desktop_{};
  bool sync_available_{};
  xcb_timestamp_t last_event_time_{};
  xcb_timestamp_t last_user_time_{};
  std::map<xcb_window_t, std::uint32_t> user_time_masks_;
  std::uint16_t screen_width_{};
  std::uint16_t screen_height_{};
  std::vector<DisplayMonitor> monitors_;
  std::uint8_t randr_event_base_{};
  // Test-only: accept synthetic root ConfigureNotify sizes for Xvfb, which
  // cannot change its RandR mode. Set MIDNIGHT_WM_TEST_SYNTHETIC_RESIZE=1.
  bool accept_synthetic_root_size_{};
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
