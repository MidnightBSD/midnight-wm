#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace midnight {

struct DbusMenuRow {
  int id{};
  int parent_id{};
  int depth{};
  std::string label;
  std::string icon_name;
  std::string toggle_type;
  int toggle_state{-1};
  bool enabled{true};
  bool visible{true};
  bool separator{};
  bool has_children{};
};

// A small asynchronous client for the com.canonical.dbusmenu interface.
// Callbacks run on the default GLib main context.
class DbusMenu {
 public:
  struct Impl;
  DbusMenu();
  ~DbusMenu();
  DbusMenu(const DbusMenu&) = delete;
  DbusMenu& operator=(const DbusMenu&) = delete;

  bool initialize();
  void set_changed_handler(std::function<void()> handler);
  void open(std::string service, std::string path, int parent_id = 0);
  void about_to_show(int id);
  void close();
  [[nodiscard]] const std::vector<DbusMenuRow>& rows() const;
  [[nodiscard]] int parent_id() const;
  [[nodiscard]] bool loaded() const;
  [[nodiscard]] bool failed() const;
  void activate(int id, std::uint32_t timestamp = 0);

 private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace midnight
