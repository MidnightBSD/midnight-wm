#pragma once

#include <xcb/xcb.h>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace midnight {

// File URI XDND version 5 helper. Route events to handle() before the normal
// shell and window-manager handlers. Callbacks run on that event-loop thread.
class X11Dnd {
public:
  using DropFinish = std::function<void(bool success)>;
  // Invoke finish on the X event-loop thread after the file operation ends.
  using DropCallback = std::function<void(xcb_window_t target,
      std::vector<std::filesystem::path> files, bool move, int root_x, int root_y,
      DropFinish finish)>;
  using DragFinished = std::function<void(bool accepted, bool moved)>;

  X11Dnd(xcb_connection_t* connection, xcb_window_t root);
  ~X11Dnd();
  X11Dnd(const X11Dnd&) = delete;
  X11Dnd& operator=(const X11Dnd&) = delete;

  [[nodiscard]] bool initialize();
  // Publishes XdndAware on top-level desktop/folder windows, including
  // override-redirect windows. The callback is invoked only after data arrives.
  void register_target(xcb_window_t window, DropCallback callback);
  void unregister_target(xcb_window_t window);
  // Starts a drag after the shell's movement threshold. Grabs the pointer;
  // the release event completes or cancels the drag.
  [[nodiscard]] bool begin_drag(xcb_window_t source, std::vector<std::filesystem::path> files,
                                bool move, DragFinished finished = {});
  [[nodiscard]] bool handle(xcb_generic_event_t* event);
  // Call once per main-loop iteration to expire unresponsive XDND targets.
  void tick();

private:
  struct Atoms {
    xcb_atom_t aware{}, selection{}, enter{}, position{}, status{}, leave{}, drop{}, finished{};
    xcb_atom_t uri{}, targets{}, incr{}, property{}, copy{}, move{}, proxy{};
  } atoms_;
  struct Target {
    DropCallback callback;
    xcb_window_t source{};
    xcb_atom_t offered{};
    bool accepted{};
    bool move{};
    bool receiving_incr{};
    bool pending_finish{};
    std::uint64_t generation{};
    std::chrono::steady_clock::time_point deadline{};
    int root_x{}, root_y{};
    std::string received;
  };
  struct Outgoing {
    xcb_window_t requestor{};
    xcb_atom_t property{};
    std::string data;
    std::size_t offset{};
  };

  xcb_connection_t* connection_{};
  xcb_window_t root_{};
  bool initialized_{};
  std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
  std::map<xcb_window_t, Target> targets_;
  std::uint64_t next_generation_{};
  std::vector<Outgoing> outgoing_;
  xcb_window_t source_{};
  xcb_window_t hover_{};
  xcb_window_t hover_proxy_{};
  std::vector<std::filesystem::path> source_files_;
  std::string source_uris_;
  bool source_move_{};
  bool hover_accepted_{};
  bool waiting_status_{};
  bool release_pending_{};
  bool dropped_{};
  int last_x_{}, last_y_{}, sent_x_{}, sent_y_{};
  xcb_timestamp_t last_time_{};
  std::chrono::steady_clock::time_point deadline_{};
  DragFinished finished_;

  [[nodiscard]] xcb_atom_t intern(const char* name) const;
  [[nodiscard]] xcb_window_t aware_window(int root_x, int root_y, xcb_window_t& proxy) const;
  [[nodiscard]] xcb_window_t proxy_for(xcb_window_t window) const;
  [[nodiscard]] bool is_aware(xcb_window_t window) const;
  void send(xcb_window_t recipient, xcb_window_t event_window, xcb_atom_t type,
            uint32_t d0 = 0, uint32_t d1 = 0, uint32_t d2 = 0,
            uint32_t d3 = 0, uint32_t d4 = 0);
  void motion(int root_x, int root_y, xcb_timestamp_t time);
  void send_position();
  void finish_source(bool accepted, bool moved);
  void send_target_status(xcb_window_t target, Target& state);
  void send_target_finished(xcb_window_t target, Target& state, bool success);
  void complete_drop(xcb_window_t target, bool success);
  void finish_target(xcb_window_t target, std::uint64_t generation, bool success);
  void handle_client(const xcb_client_message_event_t& event);
  void handle_selection_request(const xcb_selection_request_event_t& event);
  void handle_selection_notify(const xcb_selection_notify_event_t& event);
  void handle_property(const xcb_property_notify_event_t& event);
};

} // namespace midnight
