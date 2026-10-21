#pragma once

#include <xcb/xcb.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace midnight {

// Publishes desktop preferences through the per-screen XSETTINGS selection.
// An existing settings manager keeps ownership; we never replace it.
class XSettings {
public:
  XSettings(xcb_connection_t* connection, xcb_screen_t* screen, int screen_number);
  ~XSettings();

  XSettings(const XSettings&) = delete;
  XSettings& operator=(const XSettings&) = delete;

  [[nodiscard]] bool initialize();
  [[nodiscard]] bool handle(const xcb_generic_event_t* event);
  void tick();
  void set_dark_mode(bool enabled);

private:
  struct Values {
    std::string theme_name{"Adwaita"};
    std::string icon_theme{"hicolor"};
    std::string font_name{"Sans 10"};
    std::int32_t dpi{96 * 1024};
    std::int32_t double_click_time{400};
    std::int32_t double_click_distance{5};
    bool operator==(const Values&) const = default;
  };

  [[nodiscard]] xcb_atom_t intern(const std::string& name) const;
  [[nodiscard]] Values read_values() const;
  void publish(const Values& values);
  void release();

  xcb_connection_t* connection_;
  xcb_screen_t* screen_;
  int screen_number_;
  xcb_window_t owner_{};
  xcb_atom_t selection_{};
  xcb_atom_t settings_atom_{};
  xcb_atom_t manager_atom_{};
  std::uint32_t serial_{};
  Values values_;
  bool dark_mode_{};
  std::chrono::steady_clock::time_point next_check_{};
};

} // namespace midnight
