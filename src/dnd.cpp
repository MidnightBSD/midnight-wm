#include "dnd.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace midnight {
namespace {
constexpr std::size_t max_transfer = 32U * 1024U * 1024U;
constexpr std::size_t chunk_size = 64U * 1024U;

std::string uri_list(const std::vector<std::filesystem::path>& files) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (const auto& path : files) {
    result += "file://";
    for (unsigned char c : std::filesystem::absolute(path).string()) {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
        result += static_cast<char>(c);
      else {
        result += '%';
        result += hex[c >> 4];
        result += hex[c & 15];
      }
    }
    result += "\r\n";
  }
  return result;
}

std::optional<std::filesystem::path> parse_uri(std::string_view uri) {
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
        const char c = static_cast<char>((hi << 4) | lo);
        if (c == '\0') return std::nullopt;
        path += c;
        i += 2;
        continue;
      }
    }
    path += uri[i];
  }
  return std::filesystem::path(path);
}

std::vector<std::filesystem::path> parse_uris(std::string_view input) {
  std::vector<std::filesystem::path> files;
  while (!input.empty()) {
    const auto end = input.find('\n');
    auto line = input.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty() && line.front() != '#') {
      if (auto path = parse_uri(line)) files.push_back(std::move(*path));
    }
    if (end == std::string_view::npos) break;
    input.remove_prefix(end + 1);
  }
  return files;
}

uint32_t pack(int x, int y) {
  return (static_cast<uint32_t>(static_cast<uint16_t>(x)) << 16) |
         static_cast<uint16_t>(y);
}

} // namespace

X11Dnd::X11Dnd(xcb_connection_t* connection, xcb_window_t root)
    : connection_(connection), root_(root) {}

X11Dnd::~X11Dnd() {
  alive_->store(false);
  if (source_) xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
}

xcb_atom_t X11Dnd::intern(const char* name) const {
  auto cookie = xcb_intern_atom(connection_, 0, std::strlen(name), name);
  auto* reply = xcb_intern_atom_reply(connection_, cookie, nullptr);
  const auto atom = reply ? reply->atom : XCB_ATOM_NONE;
  std::free(reply);
  return atom;
}

bool X11Dnd::initialize() {
  if (initialized_) return true;
  if (!connection_ || !root_) return false;
  atoms_.aware = intern("XdndAware");
  atoms_.selection = intern("XdndSelection");
  atoms_.enter = intern("XdndEnter");
  atoms_.position = intern("XdndPosition");
  atoms_.status = intern("XdndStatus");
  atoms_.leave = intern("XdndLeave");
  atoms_.drop = intern("XdndDrop");
  atoms_.finished = intern("XdndFinished");
  atoms_.uri = intern("text/uri-list");
  atoms_.targets = intern("TARGETS");
  atoms_.incr = intern("INCR");
  atoms_.property = intern("_MIDNIGHT_WM_XDND_DATA");
  atoms_.copy = intern("XdndActionCopy");
  atoms_.move = intern("XdndActionMove");
  atoms_.proxy = intern("XdndProxy");
  initialized_ = atoms_.aware && atoms_.selection && atoms_.uri && atoms_.property;
  return initialized_;
}

void X11Dnd::register_target(xcb_window_t window, DropCallback callback) {
  if (!initialized_ || !window || !callback) return;
  targets_[window].callback = std::move(callback);
  const uint32_t version = 5;
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window, atoms_.aware,
                      XCB_ATOM_ATOM, 32, 1, &version);
  auto* attrs = xcb_get_window_attributes_reply(connection_,
                   xcb_get_window_attributes(connection_, window), nullptr);
  if (attrs) {
    const uint32_t mask = attrs->your_event_mask | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(connection_, window, XCB_CW_EVENT_MASK, &mask);
    std::free(attrs);
  }
  xcb_flush(connection_);
}

void X11Dnd::unregister_target(xcb_window_t window) {
  if (auto it = targets_.find(window); it != targets_.end() && it->second.pending_finish)
    send_target_finished(window, it->second, false);
  targets_.erase(window);
  if (initialized_ && window) xcb_delete_property(connection_, window, atoms_.aware);
}

void X11Dnd::send(xcb_window_t recipient, xcb_window_t event_window, xcb_atom_t type,
                  uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t d4) {
  xcb_client_message_event_t event{};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.format = 32;
  event.window = event_window;
  event.type = type;
  event.data.data32[0] = d0;
  event.data.data32[1] = d1;
  event.data.data32[2] = d2;
  event.data.data32[3] = d3;
  event.data.data32[4] = d4;
  xcb_send_event(connection_, 0, recipient, XCB_EVENT_MASK_NO_EVENT,
                 reinterpret_cast<const char*>(&event));
  xcb_flush(connection_);
}

bool X11Dnd::is_aware(xcb_window_t window) const {
  auto* property = xcb_get_property_reply(connection_,
      xcb_get_property(connection_, 0, window, atoms_.aware, XCB_ATOM_ATOM, 0, 1), nullptr);
  if (!property) return false;
  bool aware = property->format == 32 && xcb_get_property_value_length(property) >= 4 &&
      *static_cast<const uint32_t*>(xcb_get_property_value(property)) >= 3;
  std::free(property);
  return aware;
}

xcb_window_t X11Dnd::proxy_for(xcb_window_t window) const {
  auto* property = xcb_get_property_reply(connection_,
      xcb_get_property(connection_, 0, window, atoms_.proxy, XCB_ATOM_WINDOW, 0, 1), nullptr);
  if (!property) return window;
  xcb_window_t proxy = window;
  if (property->format == 32 && xcb_get_property_value_length(property) >= 4)
    proxy = *static_cast<const xcb_window_t*>(xcb_get_property_value(property));
  std::free(property);
  if (proxy == window || !proxy) return window;
  property = xcb_get_property_reply(connection_,
      xcb_get_property(connection_, 0, proxy, atoms_.proxy, XCB_ATOM_WINDOW, 0, 1), nullptr);
  const bool valid = property && property->format == 32 &&
      xcb_get_property_value_length(property) >= 4 &&
      *static_cast<const xcb_window_t*>(xcb_get_property_value(property)) == proxy;
  std::free(property);
  return valid ? proxy : window;
}

xcb_window_t X11Dnd::aware_window(int root_x, int root_y, xcb_window_t& proxy) const {
  proxy = XCB_WINDOW_NONE;
  auto* root_reply = xcb_query_pointer_reply(connection_, xcb_query_pointer(connection_, root_), nullptr);
  if (!root_reply) return XCB_WINDOW_NONE;
  xcb_window_t current = root_reply->child;
  std::free(root_reply);
  xcb_window_t found{};
  for (int depth = 0; current && depth < 20; ++depth) {
    const auto candidate = proxy_for(current);
    if (is_aware(candidate)) { found = current; proxy = candidate; }
    auto* reply = xcb_query_pointer_reply(connection_, xcb_query_pointer(connection_, current), nullptr);
    if (!reply) break;
    const auto child = reply->child;
    std::free(reply);
    if (!child || child == current) break;
    current = child;
  }
  (void)root_x;
  (void)root_y;
  return found;
}

bool X11Dnd::begin_drag(xcb_window_t source, std::vector<std::filesystem::path> files,
                        bool move, DragFinished finished) {
  if (!initialized_ || !source || files.empty() || source_) return false;
  auto* reply = xcb_grab_pointer_reply(connection_,
      xcb_grab_pointer(connection_, 0, source,
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
        XCB_CURRENT_TIME), nullptr);
  const bool grabbed = reply && reply->status == XCB_GRAB_STATUS_SUCCESS;
  std::free(reply);
  if (!grabbed) return false;
  source_ = source;
  source_files_ = std::move(files);
  source_uris_ = uri_list(source_files_);
  source_move_ = move;
  finished_ = std::move(finished);
  hover_ = hover_proxy_ = XCB_WINDOW_NONE;
  hover_accepted_ = waiting_status_ = release_pending_ = dropped_ = false;
  deadline_ = {};
  xcb_set_selection_owner(connection_, source_, atoms_.selection, XCB_CURRENT_TIME);
  if (feedback_) {
    auto* pointer = xcb_query_pointer_reply(connection_, xcb_query_pointer(connection_, root_), nullptr);
    if (pointer) {
      feedback_(source_files_.front(), source_move_, pointer->root_x, pointer->root_y);
      std::free(pointer);
    }
  }
  xcb_flush(connection_);
  return true;
}

void X11Dnd::motion(int root_x, int root_y, xcb_timestamp_t time) {
  last_x_ = root_x;
  last_y_ = root_y;
  last_time_ = time;
  // Move feedback away from the pointer before querying the destination.
  if (feedback_ && !release_pending_)
    feedback_(source_files_.front(), source_move_, root_x, root_y);
  xcb_window_t proxy{};
  const xcb_window_t target = aware_window(root_x, root_y, proxy);
  if (target != hover_ || proxy != hover_proxy_) {
    if (hover_) send(hover_proxy_, hover_, atoms_.leave, source_);
    hover_ = target;
    hover_proxy_ = proxy;
    hover_accepted_ = false;
    waiting_status_ = false;
    if (hover_) send(hover_proxy_, hover_, atoms_.enter, source_, 5U << 24, atoms_.uri);
  }
  if (hover_ && !waiting_status_) send_position();
}

void X11Dnd::send_position() {
  if (!hover_) return;
  send(hover_proxy_, hover_, atoms_.position, source_, 0,
       pack(last_x_, last_y_), last_time_, source_move_ ? atoms_.move : atoms_.copy);
  sent_x_ = last_x_;
  sent_y_ = last_y_;
  waiting_status_ = true;
}

void X11Dnd::finish_source(bool accepted, bool moved) {
  if (feedback_) feedback_({}, false, 0, 0);
  xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
  if (source_) xcb_set_selection_owner(connection_, XCB_WINDOW_NONE, atoms_.selection, XCB_CURRENT_TIME);
  source_ = hover_ = hover_proxy_ = XCB_WINDOW_NONE;
  source_files_.clear();
  source_uris_.clear();
  waiting_status_ = release_pending_ = dropped_ = hover_accepted_ = false;
  deadline_ = {};
  auto callback = std::move(finished_);
  finished_ = {};
  if (callback) callback(accepted, moved);
  xcb_flush(connection_);
}

void X11Dnd::send_target_status(xcb_window_t target, Target& state) {
  send(state.source, state.source, atoms_.status, target,
       state.accepted ? 3U : 2U, 0, 0,
       state.accepted ? (state.move ? atoms_.move : atoms_.copy) : XCB_ATOM_NONE);
}

void X11Dnd::send_target_finished(xcb_window_t target, Target& state, bool success) {
  send(state.source, state.source, atoms_.finished, target,
       success ? 1U : 0U,
       success ? (state.move ? atoms_.move : atoms_.copy) : XCB_ATOM_NONE);
}

void X11Dnd::complete_drop(xcb_window_t target, bool success) {
  auto it = targets_.find(target);
  if (it == targets_.end()) return;
  Target& state = it->second;
  auto files = success ? parse_uris(state.received) : std::vector<std::filesystem::path>{};
  success = !files.empty();
  if (!success || !state.callback) {
    send_target_finished(target, state, false);
    state.source = XCB_WINDOW_NONE;
    state.offered = XCB_ATOM_NONE;
    state.accepted = state.receiving_incr = false;
    state.received.clear();
    return;
  }
  state.pending_finish = true;
  state.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  const auto generation = state.generation;
  const auto move = state.move;
  const auto x = state.root_x, y = state.root_y;
  auto callback = state.callback;
  state.received.clear();
  state.receiving_incr = false;
  std::weak_ptr<std::atomic_bool> alive = alive_;
  callback(target, std::move(files), move, x, y,
           [this, alive, target, generation](bool operation_succeeded) {
             if (auto state = alive.lock(); state && state->load())
               finish_target(target, generation, operation_succeeded);
           });
}

void X11Dnd::finish_target(xcb_window_t target, std::uint64_t generation, bool success) {
  auto it = targets_.find(target);
  if (it == targets_.end() || !it->second.pending_finish ||
      it->second.generation != generation) return;
  Target& state = it->second;
  send_target_finished(target, state, success);
  state.source = XCB_WINDOW_NONE;
  state.offered = XCB_ATOM_NONE;
  state.accepted = state.receiving_incr = state.pending_finish = false;
  state.received.clear();
}

void X11Dnd::handle_client(const xcb_client_message_event_t& event) {
  if (event.format != 32) return;
  if (event.type == atoms_.status && source_ && event.window == source_) {
    if (event.data.data32[0] != hover_) return;
    hover_accepted_ = (event.data.data32[1] & 1U) != 0;
    waiting_status_ = false;
    if (release_pending_) {
      if (hover_accepted_)
        send(hover_proxy_, hover_, atoms_.drop, source_, 0, last_time_);
      else {
        send(hover_proxy_, hover_, atoms_.leave, source_);
        finish_source(false, false);
      }
      dropped_ = hover_accepted_;
      if (dropped_) deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    } else if (last_x_ != sent_x_ || last_y_ != sent_y_) send_position();
    return;
  }
  if (event.type == atoms_.finished && source_ && event.window == source_ &&
      event.data.data32[0] == hover_ && dropped_) {
    const bool accepted = (event.data.data32[1] & 1U) != 0;
    finish_source(accepted, accepted && event.data.data32[2] == atoms_.move);
    return;
  }
  auto it = targets_.find(event.window);
  if (it == targets_.end()) return;
  auto& state = it->second;
  if (event.type == atoms_.enter) {
    if (state.pending_finish) send_target_finished(event.window, state, false);
    auto callback = std::move(state.callback);
    state = Target{};
    state.callback = std::move(callback);
    state.generation = ++next_generation_;
    state.source = event.data.data32[0];
    const uint32_t version = event.data.data32[1] >> 24;
    if (version < 3 || version > 5) return;
    if (!(event.data.data32[1] & 1U)) {
      for (int i = 2; i <= 4; ++i)
        if (event.data.data32[i] == atoms_.uri) state.offered = atoms_.uri;
    } else {
      const xcb_atom_t type_list = intern("XdndTypeList");
      auto* property = xcb_get_property_reply(connection_,
          xcb_get_property(connection_, 0, state.source, type_list, XCB_ATOM_ATOM, 0, 256), nullptr);
      if (property && property->format == 32) {
        auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(property));
        const auto count = xcb_get_property_value_length(property) / 4;
        if (std::find(atoms, atoms + count, atoms_.uri) != atoms + count) state.offered = atoms_.uri;
      }
      std::free(property);
    }
  } else if (event.type == atoms_.position && state.source == event.data.data32[0]) {
    state.root_x = static_cast<int16_t>(event.data.data32[2] >> 16);
    state.root_y = static_cast<int16_t>(event.data.data32[2] & 0xFFFF);
    state.move = event.data.data32[4] == atoms_.move;
    state.accepted = state.offered == atoms_.uri && static_cast<bool>(state.callback);
    send_target_status(event.window, state);
  } else if (event.type == atoms_.leave && state.source == event.data.data32[0]) {
    if (state.pending_finish) send_target_finished(event.window, state, false);
    state.source = XCB_WINDOW_NONE;
    state.accepted = state.receiving_incr = state.pending_finish = false;
    state.received.clear();
  } else if (event.type == atoms_.drop && state.source == event.data.data32[0]) {
    if (!state.accepted) { send_target_finished(event.window, state, false); state.source = 0; return; }
    state.received.clear();
    state.receiving_incr = false;
    xcb_delete_property(connection_, event.window, atoms_.property);
    xcb_convert_selection(connection_, event.window, atoms_.selection,
                          atoms_.uri, atoms_.property, event.data.data32[2]);
    xcb_flush(connection_);
  }
}

void X11Dnd::handle_selection_request(const xcb_selection_request_event_t& request) {
  if (request.selection != atoms_.selection || request.owner != source_) return;
  const auto property = request.property == XCB_ATOM_NONE ? request.target : request.property;
  xcb_atom_t response = XCB_ATOM_NONE;
  // A property still carrying an unfinished INCR transfer must not be reused
  // for any target; replacing it would corrupt that transfer.
  const bool duplicate = std::any_of(outgoing_.begin(), outgoing_.end(),
      [&](const Outgoing& transfer) {
        return transfer.requestor == request.requestor && transfer.property == property;
      });
  if (!duplicate && request.target == atoms_.targets) {
    const xcb_atom_t supported[]{atoms_.targets, atoms_.uri};
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request.requestor,
                        property, XCB_ATOM_ATOM, 32, 2, supported);
    response = property;
  } else if (!duplicate && request.target == atoms_.uri) {
    // A requestor can leave INCR transfers unfinished. Bound the retained copies
    // across all peers.
    std::size_t retained = 0;
    for (const auto& transfer : outgoing_) retained += transfer.data.size();
    const bool rejected = source_uris_.size() > max_transfer ||
        (source_uris_.size() > chunk_size &&
         (outgoing_.size() >= 16 || source_uris_.size() > max_transfer - retained));
    if (!rejected) response = property;
    if (!rejected && source_uris_.size() <= chunk_size) {
      xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request.requestor,
                          property, atoms_.uri, 8, source_uris_.size(), source_uris_.data());
    } else if (!rejected) {
      auto* attrs = xcb_get_window_attributes_reply(connection_,
          xcb_get_window_attributes(connection_, request.requestor), nullptr);
      if (attrs) {
        const uint32_t mask = attrs->your_event_mask | XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(connection_, request.requestor, XCB_CW_EVENT_MASK, &mask);
        std::free(attrs);
      }
      const uint32_t bytes = std::min<std::size_t>(source_uris_.size(), UINT32_MAX);
      xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request.requestor,
                          property, atoms_.incr, 32, 1, &bytes);
      outgoing_.push_back({request.requestor, property, source_uris_, 0,
                           std::chrono::steady_clock::now() + std::chrono::seconds(30)});
    }
  }
  xcb_selection_notify_event_t notify{};
  notify.response_type = XCB_SELECTION_NOTIFY;
  notify.time = request.time;
  notify.requestor = request.requestor;
  notify.selection = request.selection;
  notify.target = request.target;
  notify.property = response;
  xcb_send_event(connection_, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
                 reinterpret_cast<const char*>(&notify));
  xcb_flush(connection_);
}

void X11Dnd::handle_selection_notify(const xcb_selection_notify_event_t& notify) {
  auto it = targets_.find(notify.requestor);
  if (it == targets_.end() || !it->second.source || notify.selection != atoms_.selection ||
      notify.target != atoms_.uri) return;
  if (notify.property == XCB_ATOM_NONE) { complete_drop(notify.requestor, false); return; }
  auto* property = xcb_get_property_reply(connection_,
      xcb_get_property(connection_, 0, notify.requestor, atoms_.property,
                       XCB_GET_PROPERTY_TYPE_ANY, 0, max_transfer / 4), nullptr);
  if (!property) { complete_drop(notify.requestor, false); return; }
  if (property->type == atoms_.incr) {
    it->second.receiving_incr = true;
    xcb_delete_property(connection_, notify.requestor, atoms_.property);
  } else {
    const int length = xcb_get_property_value_length(property);
    const bool valid = property->type == atoms_.uri && length >= 0 &&
                       property->bytes_after == 0 && static_cast<std::size_t>(length) <= max_transfer;
    if (valid) it->second.received.assign(static_cast<const char*>(xcb_get_property_value(property)), length);
    xcb_delete_property(connection_, notify.requestor, atoms_.property);
    std::free(property);
    complete_drop(notify.requestor, valid);
    return;
  }
  std::free(property);
  xcb_flush(connection_);
}

void X11Dnd::handle_property(const xcb_property_notify_event_t& event) {
  if (event.state == XCB_PROPERTY_NEW_VALUE && event.atom == atoms_.property) {
    auto it = targets_.find(event.window);
    if (it != targets_.end() && it->second.receiving_incr) {
      auto* property = xcb_get_property_reply(connection_,
          xcb_get_property(connection_, 1, event.window, atoms_.property,
                           XCB_GET_PROPERTY_TYPE_ANY, 0, max_transfer / 4), nullptr);
      if (!property) { complete_drop(event.window, false); return; }
      const int length = xcb_get_property_value_length(property);
      if (!length) {
        std::free(property);
        complete_drop(event.window, true);
        return;
      }
      const bool valid = property->type == atoms_.uri && length > 0 &&
          property->bytes_after == 0 && it->second.received.size() + length <= max_transfer;
      if (valid) it->second.received.append(static_cast<const char*>(xcb_get_property_value(property)), length);
      std::free(property);
      if (!valid) complete_drop(event.window, false);
      return;
    }
  }
  if (event.state != XCB_PROPERTY_DELETE) return;
  auto it = std::find_if(outgoing_.begin(), outgoing_.end(), [&](const Outgoing& item) {
    return item.requestor == event.window && item.property == event.atom;
  });
  if (it == outgoing_.end()) return;
  const auto length = std::min(chunk_size, it->data.size() - it->offset);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, it->requestor, it->property,
                      atoms_.uri, 8, length, it->data.data() + it->offset);
  it->offset += length;
  it->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  if (!length) outgoing_.erase(it);
  xcb_flush(connection_);
}

bool X11Dnd::handle(xcb_generic_event_t* event) {
  if (!initialized_ || !event) return false;
  const auto type = event->response_type & ~0x80U;
  if (type == XCB_CLIENT_MESSAGE) {
    const auto* message = reinterpret_cast<xcb_client_message_event_t*>(event);
    if (message->type == atoms_.enter || message->type == atoms_.position ||
        message->type == atoms_.status || message->type == atoms_.leave ||
        message->type == atoms_.drop || message->type == atoms_.finished) {
      handle_client(*message);
      return true;
    }
  }
  if (type == XCB_MOTION_NOTIFY && source_ && !dropped_) {
    const auto* motion_event = reinterpret_cast<xcb_motion_notify_event_t*>(event);
    if (motion_event->event == source_) {
      if (!release_pending_) motion(motion_event->root_x, motion_event->root_y, motion_event->time);
      return true;
    }
  }
  if (type == XCB_BUTTON_RELEASE && source_ && !dropped_) {
    const auto* release = reinterpret_cast<xcb_button_release_event_t*>(event);
    if (release->event == source_) {
      motion(release->root_x, release->root_y, release->time);
      if (feedback_) feedback_({}, false, 0, 0);
      xcb_ungrab_pointer(connection_, release->time);
      deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      if (!hover_) finish_source(false, false);
      else if (waiting_status_) release_pending_ = true;
      else if (hover_accepted_) {
        dropped_ = true;
        deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        send(hover_proxy_, hover_, atoms_.drop, source_, 0, release->time);
      } else {
        send(hover_proxy_, hover_, atoms_.leave, source_);
        finish_source(false, false);
      }
      return true;
    }
  }
  if (type == XCB_SELECTION_REQUEST) {
    const auto* request = reinterpret_cast<xcb_selection_request_event_t*>(event);
    if (request->selection == atoms_.selection && request->owner == source_) {
      handle_selection_request(*request);
      return true;
    }
  }
  if (type == XCB_DESTROY_NOTIFY) {
    const auto* destroy = reinterpret_cast<xcb_destroy_notify_event_t*>(event);
    if (source_ && (destroy->window == source_ || destroy->window == hover_ ||
                    destroy->window == hover_proxy_))
      finish_source(false, false);
    if (targets_.contains(destroy->window)) {
      if (targets_[destroy->window].pending_finish)
        send_target_finished(destroy->window, targets_[destroy->window], false);
      targets_.erase(destroy->window);
      return false;
    }
    for (auto& [window, state] : targets_) {
      (void)window;
      if (state.source == destroy->window) {
        state.source = XCB_WINDOW_NONE;
        state.pending_finish = state.receiving_incr = state.accepted = false;
        state.received.clear();
      }
    }
  }
  if (type == XCB_SELECTION_NOTIFY) {
    const auto* notify = reinterpret_cast<xcb_selection_notify_event_t*>(event);
    if (notify->selection == atoms_.selection && targets_.contains(notify->requestor)) {
      handle_selection_notify(*notify);
      return true;
    }
  }
  if (type == XCB_PROPERTY_NOTIFY) {
    const auto* property = reinterpret_cast<xcb_property_notify_event_t*>(event);
    if ((property->atom == atoms_.property && targets_.contains(property->window)) ||
        std::any_of(outgoing_.begin(), outgoing_.end(), [&](const Outgoing& item) {
          return item.requestor == property->window && item.property == property->atom;
        })) {
      handle_property(*property);
      return true;
    }
  }
  return false;
}

void X11Dnd::tick(std::chrono::steady_clock::time_point now) {
  std::erase_if(outgoing_, [now](const Outgoing& transfer) {
    return now >= transfer.deadline;
  });
  for (auto& [window, state] : targets_) {
    if (state.pending_finish && now >= state.deadline)
      finish_target(window, state.generation, false);
  }
  if (source_ && deadline_ != std::chrono::steady_clock::time_point{} &&
      now >= deadline_) {
    if (hover_ && !dropped_) send(hover_proxy_, hover_, atoms_.leave, source_);
    finish_source(false, false);
  }
}

} // namespace midnight
