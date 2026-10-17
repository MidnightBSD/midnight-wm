#include "dbus_menu.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace midnight {
namespace {
constexpr auto menu_interface = "com.canonical.dbusmenu";
constexpr std::size_t max_rows = 128;
constexpr int max_depth = 5;
constexpr std::size_t max_label_bytes = 512;

using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;

[[nodiscard]] Variant child(GVariant* value, gsize index) {
  return Variant(g_variant_get_child_value(value, index), g_variant_unref);
}

[[nodiscard]] std::string string_property(GVariant* properties, const char* name) {
  Variant value(g_variant_lookup_value(properties, name, G_VARIANT_TYPE_STRING),
                g_variant_unref);
  if (!value) return {};
  gsize length{};
  const auto* string = g_variant_get_string(value.get(), &length);
  auto limit = std::min<std::size_t>(length, max_label_bytes);
  while (limit > 0 && (static_cast<unsigned char>(string[limit]) & 0xc0u) == 0x80u)
    --limit;
  return std::string(string, limit);
}

[[nodiscard]] bool bool_property(GVariant* properties, const char* name, bool fallback) {
  Variant value(g_variant_lookup_value(properties, name, G_VARIANT_TYPE_BOOLEAN),
                g_variant_unref);
  return value ? g_variant_get_boolean(value.get()) : fallback;
}

[[nodiscard]] int int_property(GVariant* properties, const char* name, int fallback) {
  Variant value(g_variant_lookup_value(properties, name, G_VARIANT_TYPE_INT32),
                g_variant_unref);
  return value ? g_variant_get_int32(value.get()) : fallback;
}

// The outer tuple is the requested parent, so only its descendants become rows.
void append_rows(GVariant* node, int parent, int depth, std::vector<DbusMenuRow>& out) {
  if (depth >= max_depth || out.size() >= max_rows ||
      !g_variant_is_of_type(node, G_VARIANT_TYPE("(ia{sv}av)"))) return;
  auto descendants = child(node, 2);
  const auto count = std::min<gsize>(g_variant_n_children(descendants.get()), max_rows);
  for (gsize i = 0; i < count && out.size() < max_rows; ++i) {
    auto wrapped = child(descendants.get(), i);
    if (!g_variant_is_of_type(wrapped.get(), G_VARIANT_TYPE_VARIANT)) continue;
    Variant entry(g_variant_get_variant(wrapped.get()), g_variant_unref);
    if (!g_variant_is_of_type(entry.get(), G_VARIANT_TYPE("(ia{sv}av)"))) continue;
    auto id_value = child(entry.get(), 0);
    auto entry_properties = child(entry.get(), 1);
    auto entry_children = child(entry.get(), 2);
    DbusMenuRow row;
    row.id = g_variant_get_int32(id_value.get());
    row.parent_id = parent;
    row.depth = depth;
    row.label = string_property(entry_properties.get(), "label");
    row.icon_name = string_property(entry_properties.get(), "icon-name");
    row.toggle_type = string_property(entry_properties.get(), "toggle-type");
    row.toggle_state = int_property(entry_properties.get(), "toggle-state", -1);
    row.enabled = bool_property(entry_properties.get(), "enabled", true);
    row.visible = bool_property(entry_properties.get(), "visible", true);
    row.separator = string_property(entry_properties.get(), "type") == "separator";
    row.has_children = string_property(entry_properties.get(), "children-display") == "submenu" ||
                       g_variant_n_children(entry_children.get()) != 0;
    const int id = row.id;
    out.push_back(std::move(row));
    append_rows(entry.get(), id, depth + 1, out);
  }
}

struct Request {
  std::weak_ptr<DbusMenu::Impl> owner;
  std::uint64_t generation{};
};
}  // namespace

struct DbusMenu::Impl : std::enable_shared_from_this<Impl> {
  GDBusConnection* bus{};
  GCancellable* cancellable{};
  guint signal_id{};
  std::string service;
  std::string path;
  int parent{};
  std::uint64_t generation{};
  bool fetching{};
  bool pending{};
  bool has_loaded{};
  bool has_failed{};
  std::vector<DbusMenuRow> entries;
  std::function<void()> changed;

  ~Impl() {
    if (cancellable) g_cancellable_cancel(cancellable);
    if (bus && signal_id) g_dbus_connection_signal_unsubscribe(bus, signal_id);
    if (cancellable) g_object_unref(cancellable);
    if (bus) g_object_unref(bus);
  }

  void notify() { if (changed) changed(); }

  void refresh();

  void stop() {
    ++generation;
    if (bus && signal_id) g_dbus_connection_signal_unsubscribe(bus, signal_id);
    signal_id = 0;
    service.clear();
    path.clear();
    parent = 0;
    fetching = false;
    pending = false;
    has_loaded = false;
    has_failed = false;
    entries.clear();
    notify();
  }
};

namespace {
void layout_done(GObject* object, GAsyncResult* result, gpointer data) {
  std::unique_ptr<Request> request(static_cast<Request*>(data));
  GError* error = nullptr;
  Variant reply(g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error),
                g_variant_unref);
  if (error) g_error_free(error);
  auto self = request->owner.lock();
  if (!self || self->generation != request->generation) return;
  self->fetching = false;
  if (reply && g_variant_is_of_type(reply.get(), G_VARIANT_TYPE("(u(ia{sv}av))"))) {
    auto layout = child(reply.get(), 1);
    auto id = child(layout.get(), 0);
    std::vector<DbusMenuRow> next;
    append_rows(layout.get(), g_variant_get_int32(id.get()), 0, next);
    self->entries = std::move(next);
    self->has_loaded = true;
    self->has_failed = false;
    self->notify();
  } else {
    self->entries.clear();
    self->has_loaded = false;
    self->has_failed = true;
    self->notify();
  }
  if (self->pending) {
    self->pending = false;
    self->refresh();
  }
}

void about_done(GObject* object, GAsyncResult* result, gpointer data) {
  std::unique_ptr<Request> request(static_cast<Request*>(data));
  GError* error = nullptr;
  Variant reply(g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error),
                g_variant_unref);
  if (error) g_error_free(error);
  auto self = request->owner.lock();
  if (self && self->generation == request->generation) self->refresh();
}

void menu_signal(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                 const gchar* signal, GVariant*, gpointer data) {
  auto* self = static_cast<DbusMenu::Impl*>(data);
  if (g_strcmp0(signal, "LayoutUpdated") == 0 ||
      g_strcmp0(signal, "ItemsPropertiesUpdated") == 0) self->refresh();
}
}  // namespace

void DbusMenu::Impl::refresh() {
  if (!bus || service.empty()) return;
  if (fetching) { pending = true; return; }
  fetching = true;
  // Ask for all descendants and all properties. Parsing still has explicit limits.
  g_dbus_connection_call(bus, service.c_str(), path.c_str(), menu_interface,
                         "GetLayout", g_variant_new("(ii@as)", parent, -1,
                                                    g_variant_new_strv(nullptr, 0)),
                         G_VARIANT_TYPE("(u(ia{sv}av))"), G_DBUS_CALL_FLAGS_NONE,
                         2000, cancellable, layout_done,
                         new Request{weak_from_this(), generation});
}

DbusMenu::DbusMenu() : impl_(std::make_shared<Impl>()) {}
DbusMenu::~DbusMenu() = default;

bool DbusMenu::initialize() {
  if (impl_->bus) return true;
  GError* error = nullptr;
  impl_->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (error) g_error_free(error);
  if (!impl_->bus) return false;
  impl_->cancellable = g_cancellable_new();
  return true;
}

void DbusMenu::set_changed_handler(std::function<void()> handler) {
  impl_->changed = std::move(handler);
}

void DbusMenu::open(std::string service, std::string path, int parent_id) {
  impl_->stop();
  if (!impl_->bus || !g_dbus_is_name(service.c_str()) ||
      !g_variant_is_object_path(path.c_str()) || path == "/NO_DBUSMENU") return;
  impl_->service = std::move(service);
  impl_->path = std::move(path);
  impl_->parent = parent_id;
  impl_->signal_id = g_dbus_connection_signal_subscribe(
      impl_->bus, impl_->service.c_str(), menu_interface, nullptr, impl_->path.c_str(),
      nullptr, G_DBUS_SIGNAL_FLAGS_NONE, menu_signal, impl_.get(), nullptr);
  about_to_show(parent_id);
}

void DbusMenu::about_to_show(int id) {
  if (!impl_->bus || impl_->service.empty()) return;
  g_dbus_connection_call(impl_->bus, impl_->service.c_str(), impl_->path.c_str(),
                         menu_interface, "AboutToShow", g_variant_new("(i)", id),
                         G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1500,
                         impl_->cancellable, about_done,
                         new Request{impl_, impl_->generation});
}

void DbusMenu::close() { impl_->stop(); }

const std::vector<DbusMenuRow>& DbusMenu::rows() const { return impl_->entries; }
int DbusMenu::parent_id() const { return impl_->parent; }
bool DbusMenu::loaded() const { return impl_->has_loaded; }
bool DbusMenu::failed() const { return impl_->has_failed; }

void DbusMenu::activate(int id, std::uint32_t timestamp) {
  if (!impl_->bus || impl_->service.empty()) return;
  const auto it = std::ranges::find(impl_->entries, id, &DbusMenuRow::id);
  if (it == impl_->entries.end() || !it->enabled || !it->visible ||
      it->separator || it->has_children) return;
  g_dbus_connection_call(impl_->bus, impl_->service.c_str(), impl_->path.c_str(),
                         menu_interface, "Event",
                         g_variant_new("(isvu)", id, "clicked", g_variant_new_int32(0), timestamp),
                         nullptr, G_DBUS_CALL_FLAGS_NONE, 1500,
                         impl_->cancellable, nullptr, nullptr);
}

}  // namespace midnight
