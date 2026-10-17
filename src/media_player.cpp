// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "media_player.hpp"
#include <gio/gio.h>
#include <map>
#include <algorithm>
#include <set>
#include <string_view>
#include <utility>
namespace midnight {
namespace {
constexpr auto path = "/org/mpris/MediaPlayer2";
constexpr auto interface = "org.mpris.MediaPlayer2.Player";
constexpr std::string_view prefix = "org.mpris.MediaPlayer2.";
struct Unref { void operator()(GDBusProxy* p) const { if (p) g_object_unref(p); } };
using Proxy = std::unique_ptr<GDBusProxy, Unref>;
std::string string_property(GDBusProxy* proxy, const char* key) {
  auto* value = g_dbus_proxy_get_cached_property(proxy, key);
  std::string result;
  if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) result = g_variant_get_string(value, nullptr);
  if (value) g_variant_unref(value);
  return result;
}
bool boolean_property(GDBusProxy* proxy, const char* key) {
  auto* value = g_dbus_proxy_get_cached_property(proxy, key);
  const bool result = value && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
  if (value) g_variant_unref(value);
  return result;
}
}
struct MediaPlayers::Impl : std::enable_shared_from_this<Impl> {
  GDBusConnection* bus{};
  GCancellable* cancel{g_cancellable_new()};
  guint subscription{};
  bool stopped{};
  std::map<std::string, Proxy> proxies;
  std::set<std::string> pending;
  ~Impl() { if (bus) g_object_unref(bus); g_object_unref(cancel); }
  void discover(const std::string& name) {
    if (stopped || !std::string_view(name).starts_with(prefix) || proxies.contains(name) || pending.contains(name)) return;
    pending.insert(name);
    using Request = std::pair<std::shared_ptr<Impl>, std::string>;
    g_dbus_proxy_new(bus, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, nullptr, name.c_str(), path, interface,
      cancel, [](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Request> request(static_cast<Request*>(data));
        auto& self = *request->first;
        GError* error = nullptr;
        Proxy proxy(g_dbus_proxy_new_finish(result, &error));
        self.pending.erase(request->second);
        if (error) g_error_free(error);
        if (self.stopped || !proxy) return;
        auto* owner = g_dbus_proxy_get_name_owner(proxy.get());
        if (owner) self.proxies.insert_or_assign(request->second, std::move(proxy));
        g_free(owner);
      }, new Request(shared_from_this(), name));
  }
};
MediaPlayers::MediaPlayers() : impl_(std::make_shared<Impl>()) {}
MediaPlayers::~MediaPlayers() {
  impl_->stopped = true;
  g_cancellable_cancel(impl_->cancel);
  if (impl_->subscription) g_dbus_connection_signal_unsubscribe(impl_->bus, impl_->subscription);
}
void MediaPlayers::initialize() {
  auto& self = *impl_;
  if (self.bus) return;
  self.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  if (!self.bus) return;
  self.subscription = g_dbus_connection_signal_subscribe(self.bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
    "NameOwnerChanged", "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
    [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer data) {
      if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)"))) return;
      auto& self = *static_cast<Impl*>(data);
      const char *name{}, *old_owner{}, *new_owner{};
      g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
      if (!std::string_view(name).starts_with(prefix)) return;
      self.proxies.erase(name);
      if (*new_owner) self.discover(name);
    }, &self, nullptr);
  g_dbus_connection_call(self.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
    "ListNames", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 2000, self.cancel,
    [](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<std::shared_ptr<Impl>> self(static_cast<std::shared_ptr<Impl>*>(data));
      GError* error = nullptr;
      auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
      if (error) g_error_free(error);
      if (!reply) return;
      if (!(**self).stopped) {
        auto* names = g_variant_get_child_value(reply, 0);
        GVariantIter iter; g_variant_iter_init(&iter, names);
        const char* name;
        while (g_variant_iter_next(&iter, "&s", &name)) (**self).discover(name);
        g_variant_unref(names);
      }
      g_variant_unref(reply);
    }, new std::shared_ptr<Impl>(impl_));
}
std::vector<MediaPlayerState> MediaPlayers::players() const {
  std::vector<MediaPlayerState> result;
  for (const auto& [name, proxy] : impl_->proxies) {
    auto* owner = g_dbus_proxy_get_name_owner(proxy.get());
    if (!owner) continue;
    g_free(owner);
    MediaPlayerState state{.bus_name = name, .title = {}, .artist = {},
      .status = string_property(proxy.get(), "PlaybackStatus"),
      .can_control = boolean_property(proxy.get(), "CanControl"), .can_play = boolean_property(proxy.get(), "CanPlay"),
      .can_pause = boolean_property(proxy.get(), "CanPause"), .can_previous = boolean_property(proxy.get(), "CanGoPrevious"),
      .can_next = boolean_property(proxy.get(), "CanGoNext")};
    auto* metadata = g_dbus_proxy_get_cached_property(proxy.get(), "Metadata");
    if (metadata && g_variant_is_of_type(metadata, G_VARIANT_TYPE_VARDICT)) {
      const char* title;
      if (g_variant_lookup(metadata, "xesam:title", "&s", &title)) state.title = title;
      auto* artists = g_variant_lookup_value(metadata, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY);
      if (artists) {
        GVariantIter iter; g_variant_iter_init(&iter, artists); const char* artist;
        while (g_variant_iter_next(&iter, "&s", &artist)) {
          if (!state.artist.empty()) state.artist += ", ";
          state.artist += artist;
          if (state.artist.size() > 1024) break;
        }
        g_variant_unref(artists);
      }
    }
    if (metadata) g_variant_unref(metadata);
    const auto bounded = [](std::string& value) {
      if (value.size() > 4096) {
        auto end = std::size_t{4096};
        while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80) --end;
        value.resize(end);
      }
    };
    bounded(state.title);
    bounded(state.artist);
    result.push_back(std::move(state));
  }
  return result;
}
void MediaPlayers::command(const std::string& name, const std::string& method) {
  for (const auto& player : players()) {
    if (player.bus_name != name || !player.can_control) continue;
    if (!((method == "Next" && player.can_next) || (method == "Previous" && player.can_previous) ||
          (method == "Play" && player.can_play) || (method == "Pause" && player.can_pause))) return;
    g_dbus_proxy_call(impl_->proxies.at(name).get(), method.c_str(), nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START,
      2000, impl_->cancel, [](GObject* source, GAsyncResult* result, gpointer) {
        GError* error = nullptr;
        auto* reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
        if (reply) g_variant_unref(reply);
        if (error) { g_warning("Media control: %s", error->message); g_error_free(error); }
      }, nullptr);
    return;
  }
}
}
