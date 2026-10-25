#pragma once

#include <xcb/xcb.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace midnight {

// Drive this object from the same XCB event loop as the window manager. The
// callback passed to request_files is invoked by handle(), unless the
// selection is owned locally (when it is invoked immediately). Persistence
// after an external owner exits covers UTF8_STRING/STRING text and GNOME/URI
// file lists. Other binary clipboard formats are not cached by this version.
class X11Clipboard {
public:
  using FilesCallback = std::function<void(std::vector<std::filesystem::path>, bool cut)>;

  X11Clipboard(xcb_connection_t* connection, xcb_window_t root);
  ~X11Clipboard();
  X11Clipboard(const X11Clipboard&) = delete;
  X11Clipboard& operator=(const X11Clipboard&) = delete;

  [[nodiscard]] bool initialize();
  // Returns true for selection or property events consumed by the clipboard.
  [[nodiscard]] bool handle(xcb_generic_event_t* event);
  void tick(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void set_files(std::vector<std::filesystem::path> files, bool cut);
  void request_files(FilesCallback callback);
  [[nodiscard]] bool has_files() const;
  [[nodiscard]] const std::vector<std::filesystem::path>& get_files() const;
  [[nodiscard]] bool files_are_cut() const;

private:
  struct Atoms {
    xcb_atom_t clipboard{}, manager{}, targets{}, utf8{}, text{}, string{}, uri{}, gnome{}, incr{}, save_targets{}, atom{}, property{};
  } atoms_;
  enum class Fetch { none, snapshot_utf8, snapshot_string, snapshot_gnome, snapshot_uri, files_gnome, files_uri };
  struct Outgoing {
    xcb_window_t requestor{};
    xcb_atom_t property{};
    xcb_atom_t type{};
    std::string data;
    std::size_t offset{};
    std::chrono::steady_clock::time_point deadline{};
  };

  xcb_connection_t* connection_{};
  xcb_window_t root_{};
  xcb_window_t window_{};
  uint8_t xfixes_event_base_{};
  bool initialized_{};
  bool manager_owned_{};
  bool clipboard_owned_{};
  bool snapshotting_{};
  bool receiving_incr_{};
  bool cut_{};
  xcb_window_t snapshot_owner_{};
  Fetch fetch_{Fetch::none};
  std::chrono::steady_clock::time_point fetch_deadline_{};
  std::string received_;
  std::string text_;
  std::vector<std::filesystem::path> files_;
  FilesCallback files_callback_;
  std::vector<Outgoing> outgoing_;
  std::optional<xcb_selection_request_event_t> manager_request_;

  [[nodiscard]] xcb_atom_t intern(const char* name) const;
  void observe_owner(xcb_window_t owner);
  void start_fetch(Fetch fetch);
  void finish_fetch(bool success);
  void next_snapshot();
  void finish_snapshot();
  void consume_files(bool gnome, const std::string& data);
  void selection_request(const xcb_selection_request_event_t& request);
  void selection_notify(const xcb_selection_notify_event_t& notify);
  void property_notify(const xcb_property_notify_event_t& notify);
  void send_notify(const xcb_selection_request_event_t& request, xcb_atom_t property);
  void send_payload(const xcb_selection_request_event_t& request, xcb_atom_t property,
                    xcb_atom_t type, const std::string& payload);
};

} // namespace midnight
