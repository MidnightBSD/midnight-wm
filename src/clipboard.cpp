#include "clipboard.hpp"

#include <xcb/xfixes.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

namespace midnight {
namespace {

constexpr std::size_t max_transfer = 32U * 1024U * 1024U;
constexpr std::size_t chunk_size = 64U * 1024U;

bool unreserved(unsigned char c) {
  return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
}

std::string uri_for(const std::filesystem::path& path) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string uri = "file://";
  for (unsigned char c : std::filesystem::absolute(path).string()) {
    if (unreserved(c)) uri += static_cast<char>(c);
    else {
      uri += '%';
      uri += hex[c >> 4];
      uri += hex[c & 15];
    }
  }
  return uri;
}

std::optional<std::filesystem::path> path_for(std::string_view uri) {
  if (!uri.starts_with("file://")) return std::nullopt;
  uri.remove_prefix(7);
  if (uri.starts_with("localhost/")) uri.remove_prefix(9);
  if (uri.empty() || uri.front() != '/') return std::nullopt;
  std::string path;
  for (std::size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size()) {
      auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
      };
      const int hi = digit(uri[i + 1]), lo = digit(uri[i + 2]);
      if (hi >= 0 && lo >= 0) {
        const char decoded = static_cast<char>((hi << 4) | lo);
        if (decoded == '\0') return std::nullopt;
        path += decoded;
        i += 2;
        continue;
      }
    }
    path += uri[i];
  }
  return std::filesystem::path(path);
}

std::string uri_list(const std::vector<std::filesystem::path>& files) {
  std::string result;
  for (const auto& path : files) result += uri_for(path) + "\r\n";
  return result;
}

} // namespace

X11Clipboard::X11Clipboard(xcb_connection_t* connection, xcb_window_t root)
    : connection_(connection), root_(root) {}

X11Clipboard::~X11Clipboard() {
  if (window_ != XCB_WINDOW_NONE && connection_) xcb_destroy_window(connection_, window_);
}

xcb_atom_t X11Clipboard::intern(const char* name) const {
  auto cookie = xcb_intern_atom(connection_, 0, std::strlen(name), name);
  auto* reply = xcb_intern_atom_reply(connection_, cookie, nullptr);
  const auto atom = reply ? reply->atom : XCB_ATOM_NONE;
  std::free(reply);
  return atom;
}

bool X11Clipboard::initialize() {
  if (initialized_) return true;
  if (!connection_ || root_ == XCB_WINDOW_NONE) return false;
  const xcb_query_extension_reply_t* extension = xcb_get_extension_data(connection_, &xcb_xfixes_id);
  if (!extension || !extension->present) return false;
  xfixes_event_base_ = extension->first_event;
  auto version = xcb_xfixes_query_version(connection_, 5, 0);
  auto* version_reply = xcb_xfixes_query_version_reply(connection_, version, nullptr);
  if (!version_reply) return false;
  std::free(version_reply);

  atoms_.clipboard = intern("CLIPBOARD");
  atoms_.manager = intern("CLIPBOARD_MANAGER");
  atoms_.targets = intern("TARGETS");
  atoms_.utf8 = intern("UTF8_STRING");
  atoms_.text = intern("TEXT");
  atoms_.string = XCB_ATOM_STRING;
  atoms_.uri = intern("text/uri-list");
  atoms_.gnome = intern("x-special/gnome-copied-files");
  atoms_.incr = intern("INCR");
  atoms_.save_targets = intern("SAVE_TARGETS");
  atoms_.atom = XCB_ATOM_ATOM;
  atoms_.property = intern("_MIDNIGHT_WM_CLIPBOARD");
  if (!atoms_.clipboard || !atoms_.property || !atoms_.incr) return false;

  window_ = xcb_generate_id(connection_);
  const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_create_window(connection_, XCB_COPY_FROM_PARENT, window_, root_, 0, 0, 1, 1, 0,
                    XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
                    XCB_CW_EVENT_MASK, &mask);
  const uint32_t selection_events = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                                    XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                                    XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
  xcb_xfixes_select_selection_input(connection_, window_, atoms_.clipboard, selection_events);
  auto cookie = xcb_get_selection_owner(connection_, atoms_.manager);
  auto* manager = xcb_get_selection_owner_reply(connection_, cookie, nullptr);
  if (manager && manager->owner == XCB_WINDOW_NONE) {
    xcb_set_selection_owner(connection_, window_, atoms_.manager, XCB_CURRENT_TIME);
    manager_owned_ = true;
  }
  std::free(manager);
  cookie = xcb_get_selection_owner(connection_, atoms_.clipboard);
  auto* owner = xcb_get_selection_owner_reply(connection_, cookie, nullptr);
  initialized_ = true;
  if (owner) observe_owner(owner->owner);
  std::free(owner);
  xcb_flush(connection_);
  return true;
}

void X11Clipboard::observe_owner(xcb_window_t owner) {
  if (owner == window_) {
    clipboard_owned_ = true;
    return;
  }
  clipboard_owned_ = false;
  if (owner == XCB_WINDOW_NONE) {
    fetch_ = Fetch::none;
    receiving_incr_ = false;
    if (!text_.empty() || !files_.empty()) {
      xcb_set_selection_owner(connection_, window_, atoms_.clipboard, XCB_CURRENT_TIME);
      clipboard_owned_ = true;
      xcb_flush(connection_);
    }
    finish_snapshot();
    return;
  }
  if (owner == snapshot_owner_ && snapshotting_) return;
  snapshot_owner_ = owner;
  text_.clear();
  files_.clear();
  cut_ = false;
  snapshotting_ = true;
  receiving_incr_ = false;
  fetch_ = Fetch::none;
  start_fetch(Fetch::snapshot_utf8);
}

void X11Clipboard::start_fetch(Fetch fetch) {
  fetch_ = fetch;
  received_.clear();
  receiving_incr_ = false;
  xcb_atom_t target = XCB_ATOM_NONE;
  switch (fetch) {
    case Fetch::snapshot_utf8: target = atoms_.utf8; break;
    case Fetch::snapshot_string: target = atoms_.string; break;
    case Fetch::snapshot_gnome:
    case Fetch::files_gnome: target = atoms_.gnome; break;
    case Fetch::snapshot_uri:
    case Fetch::files_uri: target = atoms_.uri; break;
    case Fetch::none: return;
  }
  xcb_delete_property(connection_, window_, atoms_.property);
  xcb_convert_selection(connection_, window_, atoms_.clipboard, target, atoms_.property,
                        XCB_CURRENT_TIME);
  xcb_flush(connection_);
}

void X11Clipboard::consume_files(bool gnome, const std::string& data) {
  std::string_view input(data);
  if (gnome) {
    const auto eol = input.find('\n');
    if (eol == std::string_view::npos) return;
    const auto action = input.substr(0, eol);
    if (action != "copy" && action != "cut") return;
    cut_ = action == "cut";
    input.remove_prefix(eol + 1);
  }
  std::vector<std::filesystem::path> parsed;
  while (!input.empty()) {
    const auto eol = input.find('\n');
    auto line = input.substr(0, eol);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty() && line.front() != '#') {
      if (auto path = path_for(line)) parsed.push_back(std::move(*path));
    }
    if (eol == std::string_view::npos) break;
    input.remove_prefix(eol + 1);
  }
  if (!parsed.empty()) files_ = std::move(parsed);
}

void X11Clipboard::finish_fetch(bool success) {
  const Fetch done = fetch_;
  fetch_ = Fetch::none;
  receiving_incr_ = false;
  if (success) {
    switch (done) {
      case Fetch::snapshot_utf8: text_ = received_; break;
      case Fetch::snapshot_string:
        if (text_.empty()) text_ = received_;
        break;
      case Fetch::snapshot_gnome:
      case Fetch::files_gnome: consume_files(true, received_); break;
      case Fetch::snapshot_uri:
      case Fetch::files_uri: consume_files(false, received_); break;
      case Fetch::none: break;
    }
  }
  if (done == Fetch::snapshot_utf8) start_fetch(Fetch::snapshot_string);
  else if (done == Fetch::snapshot_string) start_fetch(Fetch::snapshot_gnome);
  else if (done == Fetch::snapshot_gnome) start_fetch(Fetch::snapshot_uri);
  else if (done == Fetch::snapshot_uri) finish_snapshot();
  else if (done == Fetch::files_gnome && files_.empty()) start_fetch(Fetch::files_uri);
  else if (done == Fetch::files_gnome || done == Fetch::files_uri) {
    auto callback = std::move(files_callback_);
    files_callback_ = {};
    if (callback) callback(files_, cut_);
  }
}

void X11Clipboard::finish_snapshot() {
  snapshotting_ = false;
  if (manager_request_) {
    send_notify(*manager_request_, manager_request_->property);
    manager_request_.reset();
  }
  if (files_callback_) {
    auto callback = std::move(files_callback_);
    files_callback_ = {};
    callback(files_, cut_);
  }
}

void X11Clipboard::next_snapshot() { finish_fetch(false); }

void X11Clipboard::set_files(std::vector<std::filesystem::path> files, bool cut) {
  if (!initialized_) return;
  files_ = std::move(files);
  cut_ = cut;
  text_ = uri_list(files_);
  snapshotting_ = false;
  fetch_ = Fetch::none;
  xcb_set_selection_owner(connection_, window_, atoms_.clipboard, XCB_CURRENT_TIME);
  clipboard_owned_ = true;
  xcb_flush(connection_);
}

bool X11Clipboard::has_files() const { return !files_.empty(); }
const std::vector<std::filesystem::path>& X11Clipboard::get_files() const { return files_; }
bool X11Clipboard::files_are_cut() const { return cut_; }

void X11Clipboard::request_files(FilesCallback callback) {
  if (!callback) return;
  if (!initialized_) {
    callback({}, false);
    return;
  }
  if (clipboard_owned_ || (snapshotting_ && !files_.empty())) {
    callback(files_, cut_);
    return;
  }
  files_callback_ = std::move(callback);
  if (!snapshotting_ && fetch_ == Fetch::none) {
    files_.clear();
    cut_ = false;
    start_fetch(Fetch::files_gnome);
  }
}

void X11Clipboard::send_notify(const xcb_selection_request_event_t& request,
                               xcb_atom_t property) {
  xcb_selection_notify_event_t notify{};
  notify.response_type = XCB_SELECTION_NOTIFY;
  notify.time = request.time;
  notify.requestor = request.requestor;
  notify.selection = request.selection;
  notify.target = request.target;
  notify.property = property;
  xcb_send_event(connection_, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
                 reinterpret_cast<const char*>(&notify));
  xcb_flush(connection_);
}

void X11Clipboard::send_payload(const xcb_selection_request_event_t& request,
                                xcb_atom_t property, xcb_atom_t type,
                                const std::string& payload) {
  if (payload.size() <= chunk_size) {
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request.requestor,
                        property, type, 8, payload.size(), payload.data());
  } else {
    const uint32_t bytes = std::min<std::size_t>(payload.size(), UINT32_MAX);
    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(connection_, request.requestor, XCB_CW_EVENT_MASK, &mask);
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request.requestor,
                        property, atoms_.incr, 32, 1, &bytes);
    outgoing_.push_back({request.requestor, property, type, payload, 0});
  }
  send_notify(request, property);
}

void X11Clipboard::selection_request(const xcb_selection_request_event_t& request) {
  const auto property = request.property == XCB_ATOM_NONE ? request.target : request.property;
  if (request.selection == atoms_.manager) {
    if (manager_owned_ && request.target == atoms_.save_targets) {
      if (snapshotting_) manager_request_ = request;
      else send_notify(request, property);
    } else send_notify(request, XCB_ATOM_NONE);
    return;
  }
  if (request.selection != atoms_.clipboard || !clipboard_owned_) {
    send_notify(request, XCB_ATOM_NONE);
    return;
  }
  if (request.target == atoms_.targets) {
    std::vector<xcb_atom_t> targets{atoms_.targets, atoms_.utf8, atoms_.text,
                                    atoms_.string, atoms_.uri, atoms_.gnome};
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request.requestor,
                        property, atoms_.atom, 32, targets.size(), targets.data());
    send_notify(request, property);
  } else if (request.target == atoms_.uri || request.target == atoms_.gnome) {
    if (files_.empty()) { send_notify(request, XCB_ATOM_NONE); return; }
    std::string payload = uri_list(files_);
    if (request.target == atoms_.gnome) payload.insert(0, cut_ ? "cut\n" : "copy\n");
    send_payload(request, property, request.target, payload);
  } else if (request.target == atoms_.utf8 || request.target == atoms_.text ||
             request.target == atoms_.string) {
    send_payload(request, property, request.target == atoms_.string ? atoms_.string : atoms_.utf8,
                 text_);
  } else send_notify(request, XCB_ATOM_NONE);
}

void X11Clipboard::selection_notify(const xcb_selection_notify_event_t& notify) {
  if (notify.requestor != window_ || notify.selection != atoms_.clipboard ||
      fetch_ == Fetch::none) return;
  const xcb_atom_t expected =
      fetch_ == Fetch::snapshot_utf8 ? atoms_.utf8 :
      fetch_ == Fetch::snapshot_string ? atoms_.string :
      (fetch_ == Fetch::snapshot_gnome || fetch_ == Fetch::files_gnome) ? atoms_.gnome : atoms_.uri;
  if (notify.target != expected) return;
  if (notify.property == XCB_ATOM_NONE) { next_snapshot(); return; }
  auto cookie = xcb_get_property(connection_, 0, window_, atoms_.property,
                                 XCB_GET_PROPERTY_TYPE_ANY, 0, max_transfer / 4);
  auto* reply = xcb_get_property_reply(connection_, cookie, nullptr);
  if (!reply) { finish_fetch(false); return; }
  if (reply->type == atoms_.incr) {
    receiving_incr_ = true;
    xcb_delete_property(connection_, window_, atoms_.property);
  } else {
    const int length = xcb_get_property_value_length(reply);
    if (length >= 0 && reply->bytes_after == 0 && static_cast<std::size_t>(length) <= max_transfer) {
      received_.assign(static_cast<const char*>(xcb_get_property_value(reply)), length);
      xcb_delete_property(connection_, window_, atoms_.property);
      std::free(reply);
      finish_fetch(true);
      return;
    }
    finish_fetch(false);
  }
  std::free(reply);
  xcb_flush(connection_);
}

void X11Clipboard::property_notify(const xcb_property_notify_event_t& notify) {
  if (notify.window == window_ && notify.atom == atoms_.property &&
      notify.state == XCB_PROPERTY_NEW_VALUE && receiving_incr_) {
    auto cookie = xcb_get_property(connection_, 1, window_, atoms_.property,
                                   XCB_GET_PROPERTY_TYPE_ANY, 0, max_transfer / 4);
    auto* reply = xcb_get_property_reply(connection_, cookie, nullptr);
    if (!reply) { finish_fetch(false); return; }
    const int length = xcb_get_property_value_length(reply);
    if (length == 0) {
      std::free(reply);
      finish_fetch(true);
      return;
    }
    if (length < 0 || reply->bytes_after != 0 || received_.size() + length > max_transfer) {
      std::free(reply);
      finish_fetch(false);
      return;
    }
    received_.append(static_cast<const char*>(xcb_get_property_value(reply)), length);
    std::free(reply);
    xcb_flush(connection_);
    return;
  }
  if (notify.state != XCB_PROPERTY_DELETE) return;
  auto it = std::find_if(outgoing_.begin(), outgoing_.end(), [&](const Outgoing& item) {
    return item.requestor == notify.window && item.property == notify.atom;
  });
  if (it == outgoing_.end()) return;
  const auto remaining = it->data.size() - it->offset;
  const auto length = std::min(remaining, chunk_size);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, it->requestor, it->property,
                      it->type, 8, length, it->data.data() + it->offset);
  it->offset += length;
  if (length == 0) outgoing_.erase(it);
  xcb_flush(connection_);
}

bool X11Clipboard::handle(xcb_generic_event_t* event) {
  if (!initialized_ || !event) return false;
  const uint8_t type = event->response_type & ~0x80U;
  if (type == xfixes_event_base_ + XCB_XFIXES_SELECTION_NOTIFY) {
    const auto* notify = reinterpret_cast<xcb_xfixes_selection_notify_event_t*>(event);
    if (notify->selection == atoms_.clipboard) observe_owner(notify->owner);
    return true;
  }
  if (type == XCB_SELECTION_REQUEST) {
    const auto* request = reinterpret_cast<xcb_selection_request_event_t*>(event);
    if (request->owner != window_) return false;
    selection_request(*request);
    return true;
  }
  if (type == XCB_SELECTION_NOTIFY) {
    const auto* notify = reinterpret_cast<xcb_selection_notify_event_t*>(event);
    if (notify->requestor != window_) return false;
    selection_notify(*notify);
    return true;
  }
  if (type == XCB_PROPERTY_NOTIFY) {
    const auto* notify = reinterpret_cast<xcb_property_notify_event_t*>(event);
    if (notify->window == window_ || std::any_of(outgoing_.begin(), outgoing_.end(),
        [&](const Outgoing& item) { return item.requestor == notify->window &&
                                            item.property == notify->atom; })) {
      property_notify(*notify);
      return true;
    }
  }
  return false;
}

} // namespace midnight
