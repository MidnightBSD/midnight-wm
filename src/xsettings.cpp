#include "xsettings.hpp"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace midnight {
namespace {
template <class T> using Reply = std::unique_ptr<T, decltype(&std::free)>;

template <class T> void append_native(std::vector<std::uint8_t>& bytes, T value) {
  const auto* first = reinterpret_cast<const std::uint8_t*>(&value);
  bytes.insert(bytes.end(), first, first + sizeof(value));
}

void pad(std::vector<std::uint8_t>& bytes) {
  while (bytes.size() % 4) bytes.push_back(0);
}

void append_name(std::vector<std::uint8_t>& bytes, std::uint8_t type,
                 const std::string& name, std::uint32_t serial) {
  bytes.push_back(type);
  bytes.push_back(0);
  append_native(bytes, static_cast<std::uint16_t>(name.size()));
  bytes.insert(bytes.end(), name.begin(), name.end());
  pad(bytes);
  append_native(bytes, serial);
}

void append_string(std::vector<std::uint8_t>& bytes, const std::string& name,
                   const std::string& value, std::uint32_t serial) {
  append_name(bytes, 1, name, serial);
  append_native(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
  pad(bytes);
}

void append_integer(std::vector<std::uint8_t>& bytes, const std::string& name,
                    std::int32_t value, std::uint32_t serial) {
  append_name(bytes, 0, name, serial);
  append_native(bytes, value);
}

std::string keyfile_string(GKeyFile* file, const char* key) {
  char* value = g_key_file_get_string(file, "Settings", key, nullptr);
  std::string result = value ? value : "";
  g_free(value);
  return result;
}

std::int32_t keyfile_positive_integer(GKeyFile* file, const char* key, std::int32_t fallback) {
  GError* error = nullptr;
  const auto value = g_key_file_get_integer(file, "Settings", key, &error);
  if (error) {
    g_error_free(error);
    return fallback;
  }
  return value > 0 ? value : fallback;
}

std::string gsettings_string(const char* schema_name, const char* key) {
  auto* source = g_settings_schema_source_get_default();
  auto* schema = source ? g_settings_schema_source_lookup(source, schema_name, TRUE) : nullptr;
  if (!schema) return {};
  std::string value;
  if (g_settings_schema_has_key(schema, key)) {
    auto* settings = g_settings_new_full(schema, nullptr, nullptr);
    char* raw = g_settings_get_string(settings, key);
    if (raw) value = raw;
    g_free(raw);
    g_object_unref(settings);
  }
  g_settings_schema_unref(schema);
  return value;
}
} // namespace

XSettings::XSettings(xcb_connection_t* connection, xcb_screen_t* screen, int screen_number)
  : connection_(connection), screen_(screen), screen_number_(screen_number) {}

XSettings::~XSettings() { release(); }

xcb_atom_t XSettings::intern(const std::string& name) const {
  const auto cookie = xcb_intern_atom(connection_, 0, name.size(), name.c_str());
  Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(connection_, cookie, nullptr), std::free);
  return reply ? reply->atom : XCB_ATOM_NONE;
}

XSettings::Values XSettings::read_values() const {
  Values result;
  const auto path = std::filesystem::path(g_get_user_config_dir()) / "gtk-3.0" / "settings.ini";
  auto* file = g_key_file_new();
  if (g_key_file_load_from_file(file, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
    result.theme_name = keyfile_string(file, "gtk-theme-name");
    result.icon_theme = keyfile_string(file, "gtk-icon-theme-name");
    result.font_name = keyfile_string(file, "gtk-font-name");
    result.double_click_time = keyfile_positive_integer(file, "gtk-double-click-time", 400);
    result.double_click_distance = keyfile_positive_integer(file, "gtk-double-click-distance", 5);
    // GTK stores gtk-xft-dpi in the same 1024ths-of-a-point unit as Xft/DPI.
    result.dpi = keyfile_positive_integer(file, "gtk-xft-dpi", 96 * 1024);
  } else {
    result.theme_name.clear();
    result.icon_theme.clear();
    result.font_name.clear();
  }
  g_key_file_unref(file);
  if (result.theme_name.empty())
    result.theme_name = gsettings_string("org.gnome.desktop.interface", "gtk-theme");
  if (result.theme_name.empty()) result.theme_name = "Adwaita";
  // Adwaita ships with GTK 3. Do not guess dark variants of custom themes.
  // The configured theme remains untouched and is restored in light mode.
  if (dark_mode_) result.theme_name = "Adwaita-dark";
  if (result.icon_theme.empty())
    result.icon_theme = gsettings_string("org.gnome.desktop.interface", "icon-theme");
  if (result.font_name.empty())
    result.font_name = gsettings_string("org.gnome.desktop.interface", "font-name");
  if (result.icon_theme.empty()) result.icon_theme = "hicolor";
  if (result.font_name.empty()) result.font_name = "Sans 10";
  return result;
}

void XSettings::publish(const Values& values) {
  if (!owner_) return;
  const auto current_serial = ++serial_;
  std::vector<std::uint8_t> bytes;
  bytes.reserve(256);
  bytes.push_back(std::endian::native == std::endian::little ? 0 : 1);
  bytes.insert(bytes.end(), 3, 0);
  append_native(bytes, current_serial);
  append_native(bytes, std::uint32_t{6});
  append_string(bytes, "Net/ThemeName", values.theme_name, current_serial);
  append_string(bytes, "Net/IconThemeName", values.icon_theme, current_serial);
  append_string(bytes, "Gtk/FontName", values.font_name, current_serial);
  append_integer(bytes, "Xft/DPI", values.dpi, current_serial);
  append_integer(bytes, "Net/DoubleClickTime", values.double_click_time, current_serial);
  append_integer(bytes, "Net/DoubleClickDistance", values.double_click_distance, current_serial);
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, owner_, settings_atom_, settings_atom_,
                      8, bytes.size(), bytes.data());
  values_ = values;
  xcb_flush(connection_);
}

bool XSettings::initialize() {
  selection_ = intern("_XSETTINGS_S" + std::to_string(screen_number_));
  settings_atom_ = intern("_XSETTINGS_SETTINGS");
  manager_atom_ = intern("MANAGER");
  if (!selection_ || !settings_atom_ || !manager_atom_) return false;

  // The server grab makes the owner check and claim indivisible with respect
  // to another settings manager. Existing owners are left in charge.
  xcb_grab_server(connection_);
  Reply<xcb_get_selection_owner_reply_t> previous(
      xcb_get_selection_owner_reply(connection_, xcb_get_selection_owner(connection_, selection_), nullptr),
      std::free);
  if (!previous || previous->owner != XCB_WINDOW_NONE) {
    xcb_ungrab_server(connection_);
    xcb_flush(connection_);
    return false;
  }

  owner_ = xcb_generate_id(connection_);
  constexpr std::uint32_t event_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  auto created = xcb_create_window_checked(connection_, XCB_COPY_FROM_PARENT, owner_, screen_->root,
                                           0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                                           XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &event_mask);
  if (auto* error = xcb_request_check(connection_, created)) {
    std::free(error);
    owner_ = XCB_WINDOW_NONE;
    xcb_ungrab_server(connection_);
    xcb_flush(connection_);
    return false;
  }
  publish(read_values());
  xcb_set_selection_owner(connection_, owner_, selection_, XCB_CURRENT_TIME);
  Reply<xcb_get_selection_owner_reply_t> acquired(
      xcb_get_selection_owner_reply(connection_, xcb_get_selection_owner(connection_, selection_), nullptr),
      std::free);
  const bool success = acquired && acquired->owner == owner_;
  xcb_ungrab_server(connection_);
  if (!success) {
    release();
    return false;
  }
  xcb_client_message_event_t manager{};
  manager.response_type = XCB_CLIENT_MESSAGE;
  manager.format = 32;
  manager.window = screen_->root;
  manager.type = manager_atom_;
  manager.data.data32[0] = XCB_CURRENT_TIME;
  manager.data.data32[1] = selection_;
  manager.data.data32[2] = owner_;
  xcb_send_event(connection_, 0, screen_->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                 reinterpret_cast<const char*>(&manager));
  xcb_flush(connection_);
  next_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  return true;
}

bool XSettings::handle(const xcb_generic_event_t* event) {
  if ((event->response_type & ~0x80) != XCB_SELECTION_CLEAR || !owner_) return false;
  const auto* clear = reinterpret_cast<const xcb_selection_clear_event_t*>(event);
  if (clear->owner != owner_ || clear->selection != selection_) return false;
  release();
  return true;
}

void XSettings::set_dark_mode(bool enabled) {
  if (dark_mode_ == enabled) return;
  dark_mode_ = enabled;
  if (owner_) publish(read_values());
}

void XSettings::tick() {
  if (!owner_ || std::chrono::steady_clock::now() < next_check_) return;
  next_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  const Values current = read_values();
  if (current != values_) publish(current);
}

void XSettings::release() {
  if (!owner_) return;
  Reply<xcb_get_selection_owner_reply_t> current(
      xcb_get_selection_owner_reply(connection_, xcb_get_selection_owner(connection_, selection_), nullptr),
      std::free);
  if (current && current->owner == owner_)
    xcb_set_selection_owner(connection_, XCB_WINDOW_NONE, selection_, XCB_CURRENT_TIME);
  xcb_destroy_window(connection_, owner_);
  owner_ = XCB_WINDOW_NONE;
  xcb_flush(connection_);
}

} // namespace midnight
