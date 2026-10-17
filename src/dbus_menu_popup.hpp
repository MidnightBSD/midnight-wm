#pragma once

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace midnight {
class DbusMenu;
class Shell;

class DbusMenuPopup {
public:
  DbusMenuPopup(xcb_connection_t* connection, xcb_screen_t* screen, Shell& shell, DbusMenu& menu);
  ~DbusMenuPopup();
  DbusMenuPopup(const DbusMenuPopup&) = delete;
  DbusMenuPopup& operator=(const DbusMenuPopup&) = delete;

  void open(std::string service, std::string path, int x, int y,
            std::function<void()> fallback);
  void close();
  [[nodiscard]] bool handle(xcb_generic_event_t* event);

private:
  struct Layer {
    xcb_window_t window{};
    int parent_id{};
    int x{}, y{}, height{};
    int scroll{};
    int hover{-1};
    std::vector<int> ids;
  };

  void changed();
  void show_layer(int parent_id, int x, int y, std::size_t depth);
  void close_from(std::size_t depth);
  void paint(const Layer& layer);
  void hover(std::size_t depth, int index);
  void activate(std::size_t depth, int index, std::uint32_t time);
  [[nodiscard]] int hit(int x, int y, std::size_t& depth) const;
  [[nodiscard]] int row_index(const Layer& layer, int y) const;

  xcb_connection_t* connection_{};
  xcb_screen_t* screen_{};
  Shell& shell_;
  DbusMenu& menu_;
  xcb_key_symbols_t* key_symbols_{};
  std::vector<Layer> layers_;
  std::function<void()> fallback_;
  int anchor_x_{}, anchor_y_{};
  bool pending_{};
  bool pointer_grabbed_{};
  bool keyboard_grabbed_{};
};
} // namespace midnight
