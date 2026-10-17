#include "startup_notification.hpp"

#include <gio/gdesktopappinfo.h>
#include <glib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <unistd.h>

struct MidnightLaunchContext {
  GAppLaunchContext parent;
  midnight::StartupNotification* owner{};
  std::string* id{};
};
struct MidnightLaunchContextClass { GAppLaunchContextClass parent; };
static GObjectClass* midnight_launch_context_parent_class = nullptr;

static char* midnight_launch_context_get_display(GAppLaunchContext*, GAppInfo*, GList*) {
  return g_strdup(g_getenv("DISPLAY"));
}

static char* midnight_launch_context_get_startup_notify_id(GAppLaunchContext* context,
                                                            GAppInfo* app, GList*) {
  auto* self = reinterpret_cast<MidnightLaunchContext*>(context);
  *self->id = self->owner->begin(app);
  return g_strdup(self->id->c_str());
}

static void midnight_launch_context_launch_failed(GAppLaunchContext* context, const char* id) {
  auto* self = reinterpret_cast<MidnightLaunchContext*>(context);
  if (self->owner && id) self->owner->finish(id);
}

static void midnight_launch_context_finalize(GObject* object) {
  auto* self = reinterpret_cast<MidnightLaunchContext*>(object);
  delete self->id;
  G_OBJECT_CLASS(midnight_launch_context_parent_class)->finalize(object);
}

static void midnight_launch_context_class_init(MidnightLaunchContextClass* klass) {
  midnight_launch_context_parent_class =
      G_OBJECT_CLASS(g_type_class_peek_parent(klass));
  auto* launch = G_APP_LAUNCH_CONTEXT_CLASS(klass);
  launch->get_display = midnight_launch_context_get_display;
  launch->get_startup_notify_id = midnight_launch_context_get_startup_notify_id;
  launch->launch_failed = midnight_launch_context_launch_failed;
  G_OBJECT_CLASS(klass)->finalize = midnight_launch_context_finalize;
}

static void midnight_launch_context_init(MidnightLaunchContext* self) {
  self->id = new std::string;
}

static GType midnight_launch_context_get_type() {
  static gsize type_id = 0;
  if (g_once_init_enter(&type_id)) {
    const auto registered = g_type_register_static_simple(
        G_TYPE_APP_LAUNCH_CONTEXT, "MidnightLaunchContext", sizeof(MidnightLaunchContextClass),
        [](gpointer klass, gpointer) {
          midnight_launch_context_class_init(static_cast<MidnightLaunchContextClass*>(klass));
        },
        sizeof(MidnightLaunchContext),
        [](GTypeInstance* instance, gpointer) {
          midnight_launch_context_init(reinterpret_cast<MidnightLaunchContext*>(instance));
        }, static_cast<GTypeFlags>(0));
    g_once_init_leave(&type_id, registered);
  }
  return static_cast<GType>(type_id);
}

namespace midnight {
namespace {
template <class T> using Reply = std::unique_ptr<T, decltype(&std::free)>;

std::string quote(std::string_view value) {
  std::string result = "\"";
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') result += '\\';
    result += ch;
  }
  result += '"';
  return result;
}

std::string property_string(xcb_connection_t* connection, xcb_window_t window, xcb_atom_t property,
                            xcb_atom_t type) {
  auto cookie = xcb_get_property(connection, 0, window, property, type, 0, 1024);
  Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection, cookie, nullptr), std::free);
  if (!reply || reply->format != 8 || reply->type != type) return {};
  return {static_cast<const char*>(xcb_get_property_value(reply.get())),
          static_cast<std::size_t>(xcb_get_property_value_length(reply.get()))};
}

// The only field we consume from incoming messages is ID. It may be quoted
// and escaped according to the protocol's key/value syntax.
std::string message_id(std::string_view message) {
  const auto pos = message.find("ID=");
  if (pos == std::string_view::npos || (pos && message[pos - 1] != ' ')) return {};
  std::string result;
  bool quoted = false, escaped = false;
  for (auto i = pos + 3; i < message.size(); ++i) {
    const char ch = message[i];
    if (escaped) { result += ch; escaped = false; }
    else if (ch == '\\') escaped = true;
    else if (ch == '"') quoted = !quoted;
    else if (!quoted && ch == ' ') break;
    else result += ch;
  }
  return result;
}
} // namespace

StartupNotification::StartupNotification(xcb_connection_t* connection, xcb_screen_t* screen,
                                         int screen_number)
    : connection_(connection), screen_(screen), screen_number_(screen_number) {}

xcb_atom_t StartupNotification::atom(const char* name) {
  if (auto it = atoms_.find(name); it != atoms_.end()) return it->second;
  auto cookie = xcb_intern_atom(connection_, 0, std::strlen(name), name);
  Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(connection_, cookie, nullptr), std::free);
  return atoms_[name] = reply ? reply->atom : XCB_ATOM_NONE;
}

void StartupNotification::changed() {
  if (change_handler_) change_handler_(pending_.size());
}

void StartupNotification::broadcast(const std::string& message) {
  const auto begin = atom("_NET_STARTUP_INFO_BEGIN");
  const auto continuation = atom("_NET_STARTUP_INFO");
  if (!begin || !continuation) return;
  const auto sender = xcb_generate_id(connection_);
  xcb_create_window(connection_, XCB_COPY_FROM_PARENT, sender, screen_->root, 0, 0, 1, 1, 0,
                    XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
  for (std::size_t offset = 0; offset <= message.size(); offset += 20) {
    xcb_client_message_event_t event{};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 8;
    event.window = sender;
    event.type = offset == 0 ? begin : continuation;
    const auto count = std::min<std::size_t>(20, message.size() + 1 - offset);
    if (offset < message.size()) std::memcpy(event.data.data8, message.data() + offset, count);
    xcb_send_event(connection_, 0, screen_->root, XCB_EVENT_MASK_PROPERTY_CHANGE,
                   reinterpret_cast<const char*>(&event));
  }
  xcb_destroy_window(connection_, sender);
  xcb_flush(connection_);
}

std::string StartupNotification::begin(GAppInfo* app) {
  std::array<char, 256> hostname{};
  if (gethostname(hostname.data(), hostname.size() - 1) != 0) std::strcpy(hostname.data(), "localhost");
  const auto id = std::string(hostname.data()) + "-midnight-wm-" + std::to_string(getpid()) + "-" +
                  std::to_string(++serial_) + "_TIME" + std::to_string(user_time_);
  std::string wm_class;
  if (G_IS_DESKTOP_APP_INFO(app)) {
    if (const char* value = g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(app)))
      wm_class = value;
  }
  pending_[id] = {wm_class, std::chrono::steady_clock::now()};
  std::string message = "new: ID=" + quote(id) + " NAME=" +
      quote(g_app_info_get_display_name(app) ? g_app_info_get_display_name(app) : "Application") +
      " SCREEN=" + std::to_string(screen_number_);
  if (!wm_class.empty()) message += " WMCLASS=" + quote(wm_class);
  if (G_IS_DESKTOP_APP_INFO(app)) {
    if (const char* desktop_id = g_app_info_get_id(app))
      message += " APPLICATION_ID=" + quote(desktop_id);
    else if (const char* desktop_id = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app)))
      message += " APPLICATION_ID=" + quote(desktop_id);
  }
  broadcast(message);
  changed();
  return id;
}

void StartupNotification::finish(std::string id) {
  if (!pending_.erase(id)) return;
  broadcast("remove: ID=" + quote(id));
  changed();
}

bool StartupNotification::launch(GAppInfo* app, GList* files, GError** error) {
  auto* context = static_cast<MidnightLaunchContext*>(g_object_new(midnight_launch_context_get_type(), nullptr));
  context->owner = this;
  const bool launched = g_app_info_launch(app, files, G_APP_LAUNCH_CONTEXT(context), error);
  if (!launched && context->id && !context->id->empty()) finish(*context->id);
  g_object_unref(context);
  return launched;
}

bool StartupNotification::launch_default_uri(const char* uri, GError** error) {
  auto* context = static_cast<MidnightLaunchContext*>(g_object_new(midnight_launch_context_get_type(), nullptr));
  context->owner = this;
  const bool launched = g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(context), error);
  if (!launched && context->id && !context->id->empty()) finish(*context->id);
  g_object_unref(context);
  return launched;
}

void StartupNotification::window_mapped(xcb_window_t window) {
  if (pending_.empty()) return;
  const auto id = property_string(connection_, window, atom("_NET_STARTUP_ID"), atom("UTF8_STRING"));
  if (!id.empty()) {
    finish(id);
    return;
  }
  const auto wm_class = property_string(connection_, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
  if (wm_class.empty()) return;
  const auto separator = wm_class.find('\0');
  const std::string_view name(wm_class.data(),
                              separator == std::string::npos ? wm_class.size() : separator);
  const std::string_view klass(separator == std::string::npos ? "" : wm_class.data() + separator + 1,
                               separator == std::string::npos ? 0 : wm_class.size() - separator - 1);
  for (const auto& [pending_id, pending] : pending_) {
    if (!pending.wm_class.empty() && (pending.wm_class == name || pending.wm_class == klass)) {
      finish(pending_id);
      return;
    }
  }
}

bool StartupNotification::handle(xcb_generic_event_t* generic) {
  if ((generic->response_type & ~0x80) != XCB_CLIENT_MESSAGE) return false;
  auto* event = reinterpret_cast<xcb_client_message_event_t*>(generic);
  if (event->format != 8) return false;
  if (event->type == atom("_NET_STARTUP_INFO_BEGIN")) {
    if (fragments_.size() >= 64) fragments_.clear();
    fragments_[event->window].clear();
  }
  else if (event->type != atom("_NET_STARTUP_INFO") || !fragments_.contains(event->window)) return false;
  auto& fragment = fragments_[event->window];
  const auto* bytes = reinterpret_cast<const char*>(event->data.data8);
  const auto length = strnlen(bytes, 20);
  fragment.append(bytes, length);
  if (fragment.size() > 4096) { fragments_.erase(event->window); return true; }
  if (length < 20) {
    if (g_utf8_validate(fragment.data(), fragment.size(), nullptr) &&
        fragment.starts_with("remove:")) {
      const auto id = message_id(fragment);
      if (pending_.erase(id)) changed();
    }
    fragments_.erase(event->window);
  }
  return true;
}

void StartupNotification::tick() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (now - it->second.started > std::chrono::seconds(30)) {
      const auto id = it->first;
      ++it;
      finish(id);
    } else ++it;
  }
}
} // namespace midnight
