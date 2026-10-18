#pragma once

#include <xcb/xcb.h>
#include "task_icon.hpp"
#include "session.hpp"

#include <array>
#include <cairo/cairo.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace midnight {

struct ThemeConfig;
class Store;
class StartupNotification;

struct Task {
  xcb_window_t window{};
  std::string title;
  bool active{};
  bool attention{};
  TaskIcon icon{};
};

class Shell {
public:
  Shell(xcb_connection_t* connection, xcb_screen_t* screen, Store& store);
  ~Shell();
  Shell(const Shell&) = delete;
  Shell& operator=(const Shell&) = delete;

  [[nodiscard]] bool initialize();
  [[nodiscard]] bool handle(xcb_generic_event_t* event);
  void tick();
  [[nodiscard]] bool menu_animation_active() const;
  void set_tasks(std::vector<Task> tasks);
  void set_workspace(std::uint32_t desktop, bool showing_desktop);
  [[nodiscard]] SessionSnapshot session_snapshot() const;
  void restore_session(const SessionSnapshot& snapshot);
  void set_screen_size(std::uint16_t width, std::uint16_t height);
  void refresh_display_layout();
  void apply_config(ThemeConfig config);
  void set_settings_program(std::string program);
  [[nodiscard]] const ThemeConfig& theme() const;
  [[nodiscard]] double ui_scale() const;
  [[nodiscard]] int panel_height() const;
  [[nodiscard]] int screen_width() const;
  [[nodiscard]] int screen_height() const;
  // Open a local folder, optionally revealing one of its items.
  [[nodiscard]] bool show_folder(const std::filesystem::path& path,
                                 const std::optional<std::filesystem::path>& selected = std::nullopt);
  [[nodiscard]] bool show_item_properties(const std::filesystem::path& path);
  [[nodiscard]] xcb_window_t panel_window() const;
  [[nodiscard]] xcb_window_t desktop_window() const;
  [[nodiscard]] bool normal_window(xcb_window_t window) const;
  void set_tray_width(int width);
  [[nodiscard]] int tray_width() const;
  [[nodiscard]] int tray_x() const;
  [[nodiscard]] int panel_x() const;
  [[nodiscard]] int panel_y() const;
  [[nodiscard]] const std::array<double, 3>& panel_color() const;
  // Returns a cached surface owned by the shell, or nullptr. The pointer is
  // valid only until the next tick() or apply_config(); paint it immediately
  // and do not keep it.
  [[nodiscard]] cairo_surface_t* themed_icon_surface(const std::string& name);
  [[nodiscard]] const std::array<double, 3>& accent_color() const;
  void show_session_notice(const std::string& message);
  void set_exit_handler(std::function<void()> handler);
  void set_startup_notification(StartupNotification* startup);
  [[nodiscard]] bool owns(xcb_window_t window) const;
  void set_file_clipboard_handlers(
      std::function<void(const std::vector<std::filesystem::path>&, bool)> set,
      std::function<void(std::function<void(std::vector<std::filesystem::path>, bool)>)> request);
  using DropFinish = std::function<void(bool)>;
  using DropHandler = std::function<void(xcb_window_t, std::vector<std::filesystem::path>, bool, int, int, DropFinish)>;
  void update_drag_feedback(const std::filesystem::path& path, bool move, int root_x, int root_y);
  void set_dnd_handlers(std::function<void(xcb_window_t, DropHandler)> register_target,
                        std::function<void(xcb_window_t)> unregister_target,
                        std::function<void(xcb_window_t, std::vector<std::filesystem::path>, bool)> begin_drag);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace midnight
