#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace midnight {

// IconPixmap bytes are ARGB32 in network byte order, as specified by SNI.
struct StatusNotifierPixmap {
  int width{};
  int height{};
  std::vector<std::uint8_t> argb;
};

struct StatusNotifierItem {
  std::string key;
  std::string service;
  std::string path;
  std::string status;
  std::string title;
  std::string icon_name;
  std::string attention_icon_name;
  std::string icon_theme_path;
  std::string menu_path;
  StatusNotifierPixmap pixmap;
  bool item_is_menu{};
};

// Uses the default GLib main context, which main.cpp already iterates.
class StatusNotifier {
 public:
  struct Impl;  // Internal state shared with asynchronous D-Bus callbacks.
  StatusNotifier();
  ~StatusNotifier();
  StatusNotifier(const StatusNotifier&) = delete;
  StatusNotifier& operator=(const StatusNotifier&) = delete;

  bool initialize();
  void set_changed_handler(std::function<void()> handler);
  [[nodiscard]] const std::vector<StatusNotifierItem>& items() const;
  void activate(std::string_view key, int x, int y);
  void secondary_activate(std::string_view key, int x, int y);
  void context_menu(std::string_view key, int x, int y);
  void scroll(std::string_view key, int delta, std::string_view orientation);

 private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace midnight
