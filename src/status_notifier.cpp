#include "status_notifier.hpp"

#include <gio/gio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace midnight {
namespace {
constexpr auto watcher_path = "/StatusNotifierWatcher";
constexpr auto item_path = "/StatusNotifierItem";
constexpr std::array watcher_names{"org.kde.StatusNotifierWatcher",
                                   "org.freedesktop.StatusNotifierWatcher"};
constexpr std::array watcher_ifaces{"org.kde.StatusNotifierWatcher",
                                    "org.freedesktop.StatusNotifierWatcher"};
constexpr std::array item_ifaces{"org.kde.StatusNotifierItem",
                                 "org.freedesktop.StatusNotifierItem"};
constexpr auto properties_iface = "org.freedesktop.DBus.Properties";
constexpr auto dbus_iface = "org.freedesktop.DBus";
constexpr auto dbus_path = "/org/freedesktop/DBus";
constexpr std::size_t max_pixmap_bytes = 256 * 256 * 4;
// One peer may register many distinct object paths; bound items per owner and
// overall so a hostile peer cannot swallow the panel or multiply GetAll calls.
constexpr std::size_t max_items = 64;
constexpr std::size_t max_items_per_owner = 8;
constexpr std::size_t max_hosts = 16;

constexpr auto watcher_xml = R"xml(<node>
 <interface name="org.kde.StatusNotifierWatcher">
  <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
  <method name="RegisterStatusNotifierHost"><arg type="s" direction="in"/></method>
  <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  <property name="ProtocolVersion" type="i" access="read"/>
  <signal name="StatusNotifierItemRegistered"><arg type="s"/></signal>
  <signal name="StatusNotifierItemUnregistered"><arg type="s"/></signal>
  <signal name="StatusNotifierHostRegistered"/>
 </interface>
 <interface name="org.freedesktop.StatusNotifierWatcher">
  <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
  <method name="RegisterStatusNotifierHost"><arg type="s" direction="in"/></method>
  <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  <property name="ProtocolVersion" type="i" access="read"/>
  <signal name="StatusNotifierItemRegistered"><arg type="s"/></signal>
  <signal name="StatusNotifierItemUnregistered"><arg type="s"/></signal>
  <signal name="StatusNotifierHostRegistered"/>
 </interface>
</node>)xml";

struct FetchContext {
  std::weak_ptr<StatusNotifier::Impl> owner;
  std::string key;
  unsigned interface_index{};
};
}  // namespace

struct StatusNotifier::Impl : std::enable_shared_from_this<Impl> {
  GDBusConnection* bus{};
  GCancellable* cancellable{};
  GDBusNodeInfo* introspection{};
  std::array<guint, 2> name_ids{};
  std::array<guint, 2> object_ids{};
  std::array<guint, 2> watch_ids{};
  std::array<guint, 2> watcher_signal_ids{};
  std::array<bool, 2> owns_watcher{};
  std::array<bool, 2> external_watcher_seen{};
  guint host_name_id{};
  guint name_change_id{};
  std::string host_name;
  std::vector<StatusNotifierItem> entries;
  std::function<void()> changed;
  struct ItemMeta {
    std::string owner;
    guint signal_id{};
    unsigned interface_index{};
    // One GetAll per item at a time; signals during a fetch request one more.
    bool fetching{};
    bool fetch_pending{};
  };
  std::map<std::string, ItemMeta> metadata;
  std::map<std::string, std::string> hosts;

  ~Impl() {
    if (cancellable) g_cancellable_cancel(cancellable);
    for (auto id : watch_ids) if (id) g_bus_unwatch_name(id);
    for (auto id : name_ids) if (id) g_bus_unown_name(id);
    if (host_name_id) g_bus_unown_name(host_name_id);
    if (bus) {
      for (auto id : watcher_signal_ids) if (id) g_dbus_connection_signal_unsubscribe(bus, id);
      for (auto& [key, meta] : metadata)
        if (meta.signal_id) g_dbus_connection_signal_unsubscribe(bus, meta.signal_id);
      if (name_change_id) g_dbus_connection_signal_unsubscribe(bus, name_change_id);
      for (auto id : object_ids) if (id) g_dbus_connection_unregister_object(bus, id);
    }
    if (introspection) g_dbus_node_info_unref(introspection);
    if (cancellable) g_object_unref(cancellable);
    if (bus) g_object_unref(bus);
  }

  [[nodiscard]] StatusNotifierItem* find(std::string_view key) {
    auto it = std::ranges::find(entries, key, &StatusNotifierItem::key);
    return it == entries.end() ? nullptr : &*it;
  }

  void changed_now() { if (changed) changed(); }

  void emit_item(unsigned index, const char* signal, const std::string& key) {
    if (owns_watcher[index])
      g_dbus_connection_emit_signal(bus, nullptr, watcher_path, watcher_ifaces[index],
                                    signal, g_variant_new("(s)", key.c_str()), nullptr);
  }

  void remove(std::string_view key) {
    auto it = metadata.find(std::string(key));
    if (it == metadata.end()) return;
    if (it->second.signal_id) g_dbus_connection_signal_unsubscribe(bus, it->second.signal_id);
    metadata.erase(it);
    std::erase_if(entries, [&](const auto& item) { return item.key == key; });
    for (unsigned i = 0; i < 2; ++i) emit_item(i, "StatusNotifierItemUnregistered", std::string(key));
    changed_now();
  }

  void fetch(const std::string& key, unsigned index = 0);

  void add(std::string service, std::string path, std::string owner, unsigned source) {
    if (!g_dbus_is_name(service.c_str()) || !g_variant_is_object_path(path.c_str())) return;
    std::string key = service + path;
    if (metadata.contains(key)) return;
    if (entries.size() >= max_items) return;
    const auto owned = std::ranges::count_if(metadata, [&](const auto& entry) {
      return entry.second.owner == owner;
    });
    if (static_cast<std::size_t>(owned) >= max_items_per_owner) return;
    StatusNotifierItem item;
    item.key = key;
    item.service = std::move(service);
    item.path = std::move(path);
    item.status = "Active";
    entries.push_back(std::move(item));
    auto& meta = metadata[key];
    meta.owner = std::move(owner);
    auto* signal_key = new std::pair<Impl*, std::string>(this, key);
    meta.signal_id = g_dbus_connection_signal_subscribe(
        bus, entries.back().service.c_str(), nullptr, nullptr, entries.back().path.c_str(),
        nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, const gchar* interface,
            const gchar* signal, GVariant*, gpointer data) {
          auto& [self, key] = *static_cast<std::pair<Impl*, std::string>*>(data);
          if ((g_strcmp0(interface, item_ifaces[0]) == 0 ||
               g_strcmp0(interface, item_ifaces[1]) == 0) && g_str_has_prefix(signal, "New"))
            self->fetch(key);
          else if (g_strcmp0(interface, properties_iface) == 0 &&
                   g_strcmp0(signal, "PropertiesChanged") == 0)
            self->fetch(key);
        }, signal_key,
        +[](gpointer data) { delete static_cast<std::pair<Impl*, std::string>*>(data); });
    (void)source;
    for (unsigned i = 0; i < 2; ++i) emit_item(i, "StatusNotifierItemRegistered", key);
    changed_now();
    fetch(key);
  }

  void add_registration(const char* value, const char* sender, unsigned source) {
    if (!value || !sender) return;
    if (value[0] == '/') add(sender, value, sender, source);
    else add(value, item_path, value, source);
  }

  void call_action(std::string_view key, const char* method, GVariant* parameters) {
    auto* item = find(key);
    if (!item) { if (parameters) g_variant_unref(g_variant_ref_sink(parameters)); return; }
    g_dbus_connection_call(bus, item->service.c_str(), item->path.c_str(),
                           item_ifaces[metadata.at(std::string(key)).interface_index],
                           method, parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, 1500,
                           cancellable, nullptr, nullptr);
  }
};

namespace {
void fetch_done(GObject* object, GAsyncResult* result, gpointer data) {
  std::unique_ptr<FetchContext> context(static_cast<FetchContext*>(data));
  auto self = context->owner.lock();
  if (!self) return;
  bool refetch = false;
  if (auto meta = self->metadata.find(context->key); meta != self->metadata.end()) {
    meta->second.fetching = false;
    refetch = std::exchange(meta->second.fetch_pending, false);
  }
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
  if (!reply) {
    if (error) g_error_free(error);
    if (context->interface_index == 0) self->fetch(context->key, 1);
    else self->remove(context->key);
    return;
  }
  auto* item = self->find(context->key);
  if (!item) { g_variant_unref(reply); return; }
  self->metadata.at(context->key).interface_index = context->interface_index;
  GVariant* dict = nullptr;
  g_variant_get(reply, "(@a{sv})", &dict);
  auto lookup = [dict](const char* key) {
    GVariant* value = g_variant_lookup_value(dict, key, nullptr);
    return std::unique_ptr<GVariant, decltype(&g_variant_unref)>(value, g_variant_unref);
  };
  auto string_prop = [&](const char* name, std::string& out) {
    auto value = lookup(name);
    if (value && g_variant_is_of_type(value.get(), G_VARIANT_TYPE_STRING))
      out = g_variant_get_string(value.get(), nullptr);
  };
  string_prop("Status", item->status);
  string_prop("Title", item->title);
  string_prop("IconName", item->icon_name);
  string_prop("AttentionIconName", item->attention_icon_name);
  string_prop("IconThemePath", item->icon_theme_path);
  if (auto value = lookup("Menu"); value &&
      g_variant_is_of_type(value.get(), G_VARIANT_TYPE_OBJECT_PATH))
    item->menu_path = g_variant_get_string(value.get(), nullptr);
  if (auto value = lookup("ItemIsMenu"); value &&
      g_variant_is_of_type(value.get(), G_VARIANT_TYPE_BOOLEAN))
    item->item_is_menu = g_variant_get_boolean(value.get());

  const bool attention = item->status == "NeedsAttention";
  auto pixmaps = lookup(attention ? "AttentionIconPixmap" : "IconPixmap");
  if (!pixmaps || !g_variant_is_of_type(pixmaps.get(), G_VARIANT_TYPE("a(iiay)")))
    pixmaps = lookup("IconPixmap");
  StatusNotifierPixmap best;
  if (pixmaps && g_variant_is_of_type(pixmaps.get(), G_VARIANT_TYPE("a(iiay)"))) {
    GVariantIter iter;
    g_variant_iter_init(&iter, pixmaps.get());
    gint width{}, height{};
    GVariant* bytes{};
    while (g_variant_iter_next(&iter, "(ii@ay)", &width, &height, &bytes)) {
      if (width > 0 && height > 0 && width <= 256 && height <= 256) {
        gsize length{};
        const auto* raw = static_cast<const std::uint8_t*>(
            g_variant_get_fixed_array(bytes, &length, sizeof(std::uint8_t)));
        if (raw && length == static_cast<std::size_t>(width) * height * 4 &&
            length <= max_pixmap_bytes &&
            (best.argb.empty() || std::abs(width - 22) < std::abs(best.width - 22))) {
          best.width = width;
          best.height = height;
          best.argb.assign(raw, raw + length);
        }
      }
      g_variant_unref(bytes);
    }
  }
  item->pixmap = std::move(best);
  g_variant_unref(dict);
  g_variant_unref(reply);
  self->changed_now();
  if (refetch) self->fetch(context->key, context->interface_index);
}
}  // namespace

void StatusNotifier::Impl::fetch(const std::string& key, unsigned index) {
  auto* item = find(key);
  auto meta = metadata.find(key);
  if (!item || meta == metadata.end() || index >= item_ifaces.size()) return;
  if (meta->second.fetching) { meta->second.fetch_pending = true; return; }
  meta->second.fetching = true;
  g_dbus_connection_call(bus, item->service.c_str(), item->path.c_str(), properties_iface,
                         "GetAll", g_variant_new("(s)", item_ifaces[index]),
                         G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 2000,
                         cancellable, fetch_done,
                         new FetchContext{weak_from_this(), key, index});
}

namespace {
GVariant* get_property(GDBusConnection*, const gchar*, const gchar*, const gchar* interface,
                       const gchar* property, GError**, gpointer data) {
  auto* self = static_cast<StatusNotifier::Impl*>(data);
  (void)interface;
  if (g_strcmp0(property, "ProtocolVersion") == 0) return g_variant_new_int32(0);
  if (g_strcmp0(property, "IsStatusNotifierHostRegistered") == 0)
    return g_variant_new_boolean(!self->hosts.empty());
  if (g_strcmp0(property, "RegisteredStatusNotifierItems") == 0) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
    for (const auto& entry : self->entries) g_variant_builder_add(&builder, "s", entry.key.c_str());
    return g_variant_builder_end(&builder);
  }
  return nullptr;
}

void method_call(GDBusConnection* bus, const gchar* sender, const gchar*,
                 const gchar* interface, const gchar* method, GVariant* params,
                 GDBusMethodInvocation* invocation, gpointer data) {
  auto* self = static_cast<StatusNotifier::Impl*>(data);
  unsigned index = g_strcmp0(interface, watcher_ifaces[1]) == 0 ? 1 : 0;
  const char* value = nullptr;
  g_variant_get(params, "(&s)", &value);
  if (g_strcmp0(method, "RegisterStatusNotifierItem") == 0) {
    self->add_registration(value, sender, index);
  } else if (g_strcmp0(method, "RegisterStatusNotifierHost") == 0) {
    if (value && g_dbus_is_name(value) &&
        (self->hosts.contains(value) || self->hosts.size() < max_hosts)) {
      const bool first = self->hosts.empty();
      self->hosts[value] = sender;
      if (first)
        g_dbus_connection_emit_signal(bus, nullptr, watcher_path, interface,
                                      "StatusNotifierHostRegistered", nullptr, nullptr);
    }
  } else {
    g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR,
        G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown watcher method");
    return;
  }
  g_dbus_method_invocation_return_value(invocation, nullptr);
}

const GDBusInterfaceVTable watcher_vtable{method_call, get_property, nullptr, {}};

void watcher_acquired(GDBusConnection* bus, const gchar* name, gpointer data) {
  auto* self = static_cast<StatusNotifier::Impl*>(data);
  unsigned index = g_strcmp0(name, watcher_names[1]) == 0 ? 1 : 0;
  self->owns_watcher[index] = true;
  GError* error = nullptr;
  self->object_ids[index] = g_dbus_connection_register_object(
      bus, watcher_path, self->introspection->interfaces[index], &watcher_vtable,
      self, nullptr, &error);
  if (error) g_error_free(error);
  if (self->object_ids[index] && !self->hosts.empty())
    g_dbus_connection_emit_signal(bus, nullptr, watcher_path, watcher_ifaces[index],
                                  "StatusNotifierHostRegistered", nullptr, nullptr);
}

void watcher_lost(GDBusConnection*, const gchar* name, gpointer data) {
  auto* self = static_cast<StatusNotifier::Impl*>(data);
  unsigned index = g_strcmp0(name, watcher_names[1]) == 0 ? 1 : 0;
  self->owns_watcher[index] = false;
  if (self->object_ids[index]) {
    g_dbus_connection_unregister_object(self->bus, self->object_ids[index]);
    self->object_ids[index] = 0;
  }
}

void host_acquired(GDBusConnection* bus, const gchar*, gpointer data) {
  auto* self = static_cast<StatusNotifier::Impl*>(data);
  const bool first = self->hosts.empty();
  self->hosts[self->host_name] = g_dbus_connection_get_unique_name(bus);
  if (first) {
    for (unsigned i = 0; i < 2; ++i)
      if (self->owns_watcher[i])
        g_dbus_connection_emit_signal(bus, nullptr, watcher_path, watcher_ifaces[i],
                                      "StatusNotifierHostRegistered", nullptr, nullptr);
  }
}

void host_lost(GDBusConnection*, const gchar*, gpointer data) {
  auto* self = static_cast<StatusNotifier::Impl*>(data);
  self->hosts.erase(self->host_name);
}

void existing_items_done(GObject* object, GAsyncResult* result, gpointer data) {
  auto* context = static_cast<std::pair<std::weak_ptr<StatusNotifier::Impl>, unsigned>*>(data);
  std::unique_ptr<std::pair<std::weak_ptr<StatusNotifier::Impl>, unsigned>> guard(context);
  auto self = context->first.lock();
  if (!self) return;
  GError* error = nullptr;
  GVariant* result_value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
  if (!result_value) { if (error) g_error_free(error); return; }
  GVariant* boxed = nullptr;
  g_variant_get(result_value, "(@v)", &boxed);
  GVariant* list = g_variant_get_variant(boxed);
  if (g_variant_is_of_type(list, G_VARIANT_TYPE("as"))) {
    GVariantIter iter;
    g_variant_iter_init(&iter, list);
    const char* key = nullptr;
    while (g_variant_iter_next(&iter, "&s", &key)) {
      // Watchers conventionally return the concatenation of bus name and path.
      const char* path = std::strchr(key, '/');
      if (path) self->add(std::string(key, path), path, std::string(key, path), context->second);
      else self->add(key, item_path, key, context->second);
    }
  }
  g_variant_unref(list);
  g_variant_unref(boxed);
  g_variant_unref(result_value);
}

void watcher_appeared(GDBusConnection* bus, const gchar* name, const gchar* owner, gpointer data) {
  auto* self = static_cast<StatusNotifier::Impl*>(data);
  unsigned index = g_strcmp0(name, watcher_names[1]) == 0 ? 1 : 0;
  if (g_strcmp0(owner, g_dbus_connection_get_unique_name(bus)) == 0) return;
  self->external_watcher_seen[index] = true;
  g_dbus_connection_call(bus, name, watcher_path, watcher_ifaces[index],
                         "RegisterStatusNotifierHost", g_variant_new("(s)", self->host_name.c_str()),
                         nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, self->cancellable, nullptr, nullptr);
  g_dbus_connection_call(bus, name, watcher_path, properties_iface, "Get",
                         g_variant_new("(ss)", watcher_ifaces[index], "RegisteredStatusNotifierItems"),
                         G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000,
                         self->cancellable, existing_items_done,
                         new std::pair<std::weak_ptr<StatusNotifier::Impl>, unsigned>(self->weak_from_this(), index));
}

void watcher_disappeared(GDBusConnection*, const gchar* name, gpointer data) {
  auto* self = static_cast<StatusNotifier::Impl*>(data);
  unsigned index = g_strcmp0(name, watcher_names[1]) == 0 ? 1 : 0;
  if (!self->external_watcher_seen[index]) return;
  self->external_watcher_seen[index] = false;
  if (self->name_ids[index]) g_bus_unown_name(self->name_ids[index]);
  self->name_ids[index] = g_bus_own_name_on_connection(self->bus, watcher_names[index],
      G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, watcher_acquired, watcher_lost,
      self, nullptr);
}

void watcher_signal(GDBusConnection*, const gchar*, const gchar*,
                    const gchar* interface, const gchar* signal, GVariant* params, gpointer data) {
  // Subscribed signal bodies are not type-checked; the sender may be any
  // process that owns the external watcher name.
  if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(s)"))) return;
  auto* self = static_cast<StatusNotifier::Impl*>(data);
  unsigned index = g_strcmp0(interface, watcher_ifaces[1]) == 0 ? 1 : 0;
  const char* key = nullptr;
  if (g_strcmp0(signal, "StatusNotifierItemRegistered") == 0) {
    g_variant_get(params, "(&s)", &key);
    const char* path = std::strchr(key, '/');
    if (path) self->add(std::string(key, path), path, std::string(key, path), index);
    else self->add(key, item_path, key, index);
  } else if (g_strcmp0(signal, "StatusNotifierItemUnregistered") == 0) {
    g_variant_get(params, "(&s)", &key);
    if (std::strchr(key, '/')) self->remove(key);
    else self->remove(std::string(key) + item_path);
  }
}

void name_owner_changed(GDBusConnection*, const gchar*, const gchar*,
                        const gchar*, const gchar*, GVariant* params, gpointer data) {
  if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(sss)"))) return;
  auto* self = static_cast<StatusNotifier::Impl*>(data);
  const char *name{}, *old_owner{}, *new_owner{};
  g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (*new_owner) return;
  std::vector<std::string> removed;
  for (const auto& [key, meta] : self->metadata)
    if (meta.owner == name || meta.owner == old_owner) removed.push_back(key);
  for (const auto& key : removed) self->remove(key);
  for (auto it = self->hosts.begin(); it != self->hosts.end();) {
    if (it->first == name || it->second == name || it->second == old_owner)
      it = self->hosts.erase(it);
    else ++it;
  }
}
}  // namespace

StatusNotifier::StatusNotifier() : impl_(std::make_shared<Impl>()) {}
StatusNotifier::~StatusNotifier() = default;

bool StatusNotifier::initialize() {
  auto& self = *impl_;
  if (self.bus) return true;
  GError* error = nullptr;
  self.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (error) g_clear_error(&error);
  if (!self.bus) return false;
  self.cancellable = g_cancellable_new();
  self.introspection = g_dbus_node_info_new_for_xml(watcher_xml, &error);
  if (error) g_clear_error(&error);
  if (!self.introspection) return false;
  self.host_name = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
  self.host_name_id = g_bus_own_name_on_connection(self.bus, self.host_name.c_str(),
      G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, host_acquired, host_lost, &self, nullptr);
  for (unsigned i = 0; i < 2; ++i) {
    self.name_ids[i] = g_bus_own_name_on_connection(self.bus, watcher_names[i],
        G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, watcher_acquired, watcher_lost,
        &self, nullptr);
    self.watch_ids[i] = g_bus_watch_name_on_connection(self.bus, watcher_names[i],
        G_BUS_NAME_WATCHER_FLAGS_NONE, watcher_appeared, watcher_disappeared,
        &self, nullptr);
    self.watcher_signal_ids[i] = g_dbus_connection_signal_subscribe(
        self.bus, watcher_names[i], watcher_ifaces[i], nullptr, watcher_path, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, watcher_signal, &self, nullptr);
  }
  self.name_change_id = g_dbus_connection_signal_subscribe(
      self.bus, dbus_iface, dbus_iface, "NameOwnerChanged", dbus_path, nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, name_owner_changed, &self, nullptr);
  return true;
}

void StatusNotifier::set_changed_handler(std::function<void()> handler) {
  impl_->changed = std::move(handler);
}

const std::vector<StatusNotifierItem>& StatusNotifier::items() const { return impl_->entries; }
void StatusNotifier::activate(std::string_view key, int x, int y) {
  impl_->call_action(key, "Activate", g_variant_new("(ii)", x, y));
}
void StatusNotifier::secondary_activate(std::string_view key, int x, int y) {
  impl_->call_action(key, "SecondaryActivate", g_variant_new("(ii)", x, y));
}
void StatusNotifier::context_menu(std::string_view key, int x, int y) {
  impl_->call_action(key, "ContextMenu", g_variant_new("(ii)", x, y));
}
void StatusNotifier::scroll(std::string_view key, int delta, std::string_view orientation) {
  if (orientation != "vertical" && orientation != "horizontal") return;
  impl_->call_action(key, "Scroll", g_variant_new("(is)", delta, std::string(orientation).c_str()));
}
}  // namespace midnight
