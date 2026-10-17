#pragma once

#include <xcb/xcb.h>

#include <array>
#include <cairo/cairo.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace midnight {

class Store;

struct Task {
  xcb_window_t window{};
  std::string title;
  bool active{};
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
  void set_tasks(std::vector<Task> tasks);
  void set_screen_size(std::uint16_t width, std::uint16_t height);
  [[nodiscard]] xcb_window_t panel_window() const;
  void set_tray_width(int width);
  [[nodiscard]] int tray_width() const;
  [[nodiscard]] int tray_x() const;
  [[nodiscard]] const std::array<double, 3>& panel_color() const;
  [[nodiscard]] cairo_surface_t* themed_icon_surface(const std::string& name);
  [[nodiscard]] const std::array<double, 3>& accent_color() const;
  void set_exit_handler(std::function<void()> handler);
  [[nodiscard]] bool owns(xcb_window_t window) const;
  void set_file_clipboard_handlers(
      std::function<void(const std::vector<std::filesystem::path>&, bool)> set,
      std::function<void(std::function<void(std::vector<std::filesystem::path>, bool)>)> request);
  using DropFinish = std::function<void(bool)>;
  using DropHandler = std::function<void(xcb_window_t, std::vector<std::filesystem::path>, bool, int, int, DropFinish)>;
  void set_dnd_handlers(std::function<void(xcb_window_t, DropHandler)> register_target,
                        std::function<void(xcb_window_t)> unregister_target,
                        std::function<void(xcb_window_t, std::vector<std::filesystem::path>, bool)> begin_drag);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace midnight
