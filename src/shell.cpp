#include "shell.hpp"

#include "audio.hpp"
#include "browser_launcher.hpp"
#include "config.hpp"
#include "desktop_launcher.hpp"
#include "display_scale.hpp"
#include "display_monitors.hpp"
#include "wallpaper.hpp"
#include "model.hpp"
#include "recent_files.hpp"
#include "store.hpp"
#include "startup_notification.hpp"
#include "terminal_launcher.hpp"
#include "thumbnail.hpp"
#include "ui_theme.hpp"

#include <cairo/cairo-xcb.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <garcon/garcon.h>
#include <pango/pangocairo.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <numbers>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace midnight {
namespace {
constexpr int volume_group_width = 102;
constexpr int volume_clock_gap = 4;
constexpr int lock_button_x = 124;
constexpr int lock_button_width = 24;
constexpr int terminal_button_x = 152;
constexpr int terminal_button_width = 24;
constexpr int browser_button_x = 180;
constexpr int browser_button_width = 24;
constexpr int task_start_x = 212;
constexpr int task_button_width = 110;
constexpr int task_step = 114;
constexpr int task_overflow_width = 24;
constexpr int exit_dialog_width = 380;
constexpr int exit_dialog_height = 174;
constexpr int exit_dialog_button_y = 136;
constexpr int row_height = 24;
constexpr int icon_width = 90;
constexpr int icon_height = 90;
constexpr int folder_header = 55;
constexpr int scrollbar_width = 18;
constexpr int launcher_icon_size = 18;
constexpr int icon_chooser_width = 580;
constexpr int icon_chooser_height = 454;
constexpr int icon_chooser_columns = 5;
constexpr int icon_chooser_rows = 4;
constexpr int icon_chooser_cell_width = 112;
constexpr int icon_chooser_cell_height = 74;
constexpr int icon_chooser_page_size = icon_chooser_columns * icon_chooser_rows;
constexpr int open_with_width = 460;
constexpr int open_with_height = 390;
constexpr int open_with_rows = 10;

using Item = ShellItem;

int launcher_menu_width(int screen_width) {
  return std::clamp((screen_width - 3) / 3, 150, 250);
}

std::vector<std::string> icon_theme_list(GKeyFile* file, const char* group, const char* key) {
  std::vector<std::string> values;
  char* raw = g_key_file_get_string(file, group, key, nullptr);
  if (!raw) return values;
  // Icon Theme lists use commas, unlike the semicolon lists in desktop entries.
  for (char* part = raw; part && *part;) {
    char* next = std::strchr(part, ',');
    if (next) *next++ = '\0';
    if (*g_strstrip(part)) values.emplace_back(part);
    part = next;
  }
  g_free(raw);
  return values;
}

std::string selected_icon_theme() {
  const auto settings = std::filesystem::path(g_get_user_config_dir()) / "gtk-3.0/settings.ini";
  auto* file = g_key_file_new();
  std::string theme;
  if (g_key_file_load_from_file(file, settings.c_str(), G_KEY_FILE_NONE, nullptr)) {
    char* value = g_key_file_get_string(file, "Settings", "gtk-icon-theme-name", nullptr);
    if (value) { theme = value; g_free(value); }
  }
  g_key_file_unref(file);
  if (!theme.empty()) return theme;

  auto* source = g_settings_schema_source_get_default();
  auto* schema = source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : nullptr;
  if (schema && g_settings_schema_has_key(schema, "icon-theme")) {
    auto* settings_object = g_settings_new_full(schema, nullptr, nullptr);
    char* value = g_settings_get_string(settings_object, "icon-theme");
    if (value) { theme = value; g_free(value); }
    g_object_unref(settings_object);
  }
  if (schema) g_settings_schema_unref(schema);
  return theme.empty() ? "hicolor" : theme;
}

struct IconDirectory {
  std::filesystem::path relative;
  int size{launcher_icon_size};
  int min_size{launcher_icon_size};
  int max_size{launcher_icon_size};
  int threshold{2};
  int scale{1};
  std::string type{"Threshold"};

  int distance(int wanted) const {
    int low = (size - threshold) * scale;
    int high = (size + threshold) * scale;
    if (type == "Fixed") low = high = size * scale;
    else if (type == "Scalable") { low = min_size * scale; high = max_size * scale; }
    return wanted < low ? low - wanted : wanted > high ? wanted - high : 0;
  }
};

struct IconTheme {
  std::vector<IconDirectory> directories;
  std::vector<std::string> inherits;
};

struct Folder {
  xcb_window_t window{};
  std::filesystem::path path;
  int x{}, y{}, width{620}, height{430};
  int selected{-1};
  int scroll_rows{};
  std::vector<Item> items;
  bool trash{};
  bool sort_by_type{};
};

struct FolderScroll {
  int x{}, view_width{}, view_height{};
  int track_y{}, track_height{};
  int thumb_y{}, thumb_height{};
  int visible_rows{}, max_scroll{};
};

struct TaskLayout {
  int overflow_x{};
  std::size_t visible{};
  bool overflow{};
};

FolderScroll folder_scroll(const Folder& folder) {
  FolderScroll scroll;
  scroll.x = folder.width - 5 - scrollbar_width;
  scroll.view_width = folder.width - 10 - scrollbar_width;
  scroll.view_height = folder.height - folder_header - 22;
  const int columns = std::max(1, scroll.view_width / icon_width);
  const int rows = static_cast<int>((folder.items.size() + columns - 1) / columns);
  scroll.visible_rows = std::max(1, (scroll.view_height - 11) / icon_height);
  scroll.max_scroll = std::max(0, rows - scroll.visible_rows);
  scroll.track_y = folder_header + scrollbar_width;
  scroll.track_height = std::max(1, scroll.view_height - 2 * scrollbar_width);
  scroll.thumb_height = std::min(scroll.track_height,
    std::max(24, scroll.track_height * scroll.visible_rows / std::max(1, rows)));
  const int travel = scroll.track_height - scroll.thumb_height;
  scroll.thumb_y = scroll.track_y + (scroll.max_scroll ?
    travel * std::clamp(folder.scroll_rows, 0, scroll.max_scroll) / scroll.max_scroll : 0);
  return scroll;
}

struct Properties {
  xcb_window_t window{};
  Item item;
  xcb_window_t owner{};
  std::array<std::string, 3> values;
  int field{};
  bool select_all{};
  xcb_timestamp_t last_click_time{};
  int last_click_field{-1};
};

struct IconChooser {
  xcb_window_t window{};
  std::vector<std::string> names;
  std::vector<std::size_t> visible;
  std::string search;
  std::string selected;
  std::size_t page{};
};

struct OpenWithDialog {
  xcb_window_t window{};
  xcb_window_t owner{};
  std::filesystem::path path;
  std::string content_type;
  std::vector<GAppInfo*> apps;
  GAppInfo* default_app{};
  std::size_t scroll{};
  std::size_t selected{};
};

struct AppCategory {
  std::string label;
  std::string icon;
  std::vector<std::size_t> apps;
};

struct CategoryDefinition {
  std::string_view key;
  std::string_view label;
  std::string_view icon;
};

constexpr std::array<CategoryDefinition, 12> app_category_definitions{{
  {"Utility", "Accessories", "applications-accessories"},
  {"Development", "Development", "applications-development"},
  {"Education", "Education", "applications-education"},
  {"Game", "Games", "applications-games"},
  {"Graphics", "Graphics", "applications-graphics"},
  {"HealthFitness", "Health & Fitness", "applications-science"},
  {"Network", "Internet", "applications-internet"},
  {"AudioVideo", "Multimedia", "applications-multimedia"},
  {"Office", "Office", "applications-office"},
  {"Science", "Science", "applications-science"},
  {"Settings", "Settings", "preferences-system"},
  {"System", "System", "applications-system"}
}};

bool has_app_category(std::string_view categories, std::string_view category) {
  while (!categories.empty()) {
    const auto end = categories.find(';');
    if (categories.substr(0, end) == category) return true;
    if (end == std::string_view::npos) break;
    categories.remove_prefix(end + 1);
  }
  return false;
}

struct TransferWork {
  std::vector<std::filesystem::path> paths;
  std::filesystem::path destination;
  bool move{};
};

struct TransferCompletion {
  std::shared_ptr<bool> alive;
  std::function<void(std::expected<void, std::string>)> callback;
};

enum class MenuAction { noop, open_home, open_desktop, open_trash, new_folder, paste, show_programs,
                        show_recent, open_recent, settings,
                        show_category, next_categories, previous_categories,
                        launch, next_apps, previous_apps,
                        open_item, open_with, copy, cut, create_shadow, create_template, properties, trash,
                        restore, delete_permanently, close_folder, exit_session, refresh_folder,
                        sort_by_name, sort_by_type, activate_task, next_tasks, previous_tasks };
struct MenuEntry { std::string label; MenuAction action; std::size_t index{}; xcb_window_t window{}; };

std::string basename(const std::filesystem::path& path) {
  auto name = path.filename().string();
  return name.empty() ? path.string() : name;
}

std::string clock_text(const std::string& format) {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&time, &local);
  char text[32]{};
  std::strftime(text, sizeof(text), format.c_str(), &local);
  return text;
}

std::filesystem::path unique_path(const std::filesystem::path& desired) {
  return ShellModel::unique_path(desired);
}

std::expected<void, std::string> copy_tree(const std::filesystem::path& from,
                                           const std::filesystem::path& to) {
  return ShellModel::copy_path(from, to);
}

void fill(cairo_t* cr, double r, double g, double b, int x, int y, int w, int h) {
  cairo_set_source_rgb(cr, r, g, b);
  cairo_rectangle(cr, x, y, w, h);
  cairo_fill(cr);
}

void draw_bevel(cairo_t* cr, int x, int y, int w, int h, bool sunken = false,
                const ThemeConfig* theme = nullptr) {
  cairo_set_line_width(cr, 1);
  const auto light = theme && theme->dark_mode ? theme->highlight_color : std::array<double, 3>{1, 1, 1};
  const auto dark = theme && theme->dark_mode ? theme->shadow_color : std::array<double, 3>{.25, .25, .25};
  const auto& top = sunken ? dark : light;
  cairo_set_source_rgb(cr, top[0], top[1], top[2]);
  cairo_move_to(cr, x + w - 1, y);
  cairo_line_to(cr, x, y);
  cairo_line_to(cr, x, y + h - 1);
  cairo_stroke(cr);
  const auto& bottom = sunken ? light : dark;
  cairo_set_source_rgb(cr, bottom[0], bottom[1], bottom[2]);
  cairo_move_to(cr, x + w - 1, y);
  cairo_line_to(cr, x + w - 1, y + h - 1);
  cairo_line_to(cr, x, y + h - 1);
  cairo_stroke(cr);
}

void draw_text(cairo_t* cr, const std::string& value, int x, int y, int width,
          int size = 11, bool white = false, bool bold = false,
          const ThemeConfig& theme = ThemeConfig{}) {
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = make_ui_font(theme, bold, size, true);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, value.c_str(), -1);
  pango_layout_set_width(layout, width * PANGO_SCALE);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  const auto color = white ? std::array<double, 3>{1, 1, 1} :
    (theme.dark_mode ? theme.text_color : std::array<double, 3>{.04, .04, .04});
  cairo_set_source_rgb(cr, color[0], color[1], color[2]);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(font);
  g_object_unref(layout);
}

enum class SoundIcon { quieter, louder, mute };

void sound_icon(cairo_t* cr, int x, int y, SoundIcon kind, const ThemeConfig& theme) {
  cairo_save(cr);
  const auto color = theme.dark_mode ? theme.text_color : std::array<double, 3>{.12, .12, .12};
  cairo_set_source_rgb(cr, color[0], color[1], color[2]);
  cairo_rectangle(cr, x + 1, y + 6, 4, 5);
  cairo_fill(cr);
  cairo_move_to(cr, x + 5, y + 6);
  cairo_line_to(cr, x + 9, y + 2);
  cairo_line_to(cr, x + 9, y + 15);
  cairo_line_to(cr, x + 5, y + 11);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_set_line_width(cr, 1.5);
  if (kind == SoundIcon::mute) {
    cairo_set_source_rgb(cr, theme.dark_mode ? 1 : .65, theme.dark_mode ? .40 : .09, theme.dark_mode ? .40 : .09);
    cairo_move_to(cr, x + 11, y + 5);
    cairo_line_to(cr, x + 16, y + 12);
    cairo_move_to(cr, x + 16, y + 5);
    cairo_line_to(cr, x + 11, y + 12);
  } else {
    cairo_move_to(cr, x + 11, y + 8);
    cairo_line_to(cr, x + 16, y + 8);
    if (kind == SoundIcon::louder) {
      cairo_move_to(cr, x + 13.5, y + 5.5);
      cairo_line_to(cr, x + 13.5, y + 10.5);
    }
  }
  cairo_stroke(cr);
  cairo_restore(cr);
}

void lock_icon(cairo_t* cr, int x, int y, const ThemeConfig& theme) {
  cairo_save(cr);
  const auto color = theme.dark_mode ? theme.text_color : std::array<double, 3>{.12, .12, .12};
  cairo_set_source_rgb(cr, color[0], color[1], color[2]);
  cairo_set_line_width(cr, 2);
  cairo_arc(cr, x + 8, y + 7, 5, std::numbers::pi, 2 * std::numbers::pi);
  cairo_stroke(cr);
  cairo_rectangle(cr, x + 2, y + 7, 12, 9);
  cairo_fill(cr);
  const auto keyhole = theme.dark_mode ? theme.field_color : std::array<double, 3>{.85, .85, .85};
  cairo_set_source_rgb(cr, keyhole[0], keyhole[1], keyhole[2]);
  cairo_arc(cr, x + 8, y + 11, 1.2, 0, 2 * std::numbers::pi);
  cairo_fill(cr);
  cairo_restore(cr);
}

void browser_icon(cairo_t* cr, int x, int y, const ThemeConfig& theme) {
  cairo_save(cr);
  const auto color = theme.dark_mode ? theme.text_color : std::array<double, 3>{.13, .20, .40};
  cairo_set_source_rgb(cr, color[0], color[1], color[2]);
  cairo_set_line_width(cr, 1.4);
  cairo_arc(cr, x + 8, y + 8, 7, 0, 2 * std::numbers::pi);
  cairo_move_to(cr, x + 1, y + 8);
  cairo_line_to(cr, x + 15, y + 8);
  cairo_move_to(cr, x + 8, y + 1);
  cairo_line_to(cr, x + 8, y + 15);
  cairo_stroke(cr);
  cairo_move_to(cr, x + 8, y + 1);
  cairo_curve_to(cr, x + 3, y + 5, x + 3, y + 11, x + 8, y + 15);
  cairo_move_to(cr, x + 8, y + 1);
  cairo_curve_to(cr, x + 13, y + 5, x + 13, y + 11, x + 8, y + 15);
  cairo_stroke(cr);
  cairo_restore(cr);
}

void terminal_icon(cairo_t* cr, int x, int y) {
  cairo_save(cr);
  cairo_set_source_rgb(cr, .16, .16, .20);
  cairo_rectangle(cr, x + 1, y + 1, 16, 12);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, .89, .89, .83);
  cairo_rectangle(cr, x + 2.5, y + 2.5, 13, 9);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, .16, .16, .20);
  cairo_set_line_width(cr, 1.4);
  cairo_move_to(cr, x + 4, y + 5);
  cairo_line_to(cr, x + 7, y + 7);
  cairo_line_to(cr, x + 4, y + 9);
  cairo_move_to(cr, x + 9, y + 9);
  cairo_line_to(cr, x + 13, y + 9);
  cairo_stroke(cr);
  cairo_rectangle(cr, x + 7, y + 13, 4, 2);
  cairo_fill(cr);
  cairo_restore(cr);
}

cairo_surface_t* image_surface(const std::filesystem::path& path, int size) {
  GError* error = nullptr;
  auto* pixbuf = gdk_pixbuf_new_from_file_at_scale(path.c_str(), size, size, TRUE, &error);
  if (error) g_error_free(error);
  if (!pixbuf) return nullptr;
  const int w = gdk_pixbuf_get_width(pixbuf);
  const int h = gdk_pixbuf_get_height(pixbuf);
  auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  auto* pixels = gdk_pixbuf_get_pixels(pixbuf);
  auto* output = cairo_image_surface_get_data(surface);
  const int channels = gdk_pixbuf_get_n_channels(pixbuf);
  const int source_stride = gdk_pixbuf_get_rowstride(pixbuf);
  const int target_stride = cairo_image_surface_get_stride(surface);
  for (int y = 0; y < h; ++y) {
    auto* target = reinterpret_cast<std::uint32_t*>(output + y * target_stride);
    for (int x = 0; x < w; ++x) {
      const auto* pixel = pixels + y * source_stride + x * channels;
      const std::uint32_t alpha = channels == 4 ? pixel[3] : 255;
      target[x] = alpha << 24 | (pixel[0] * alpha / 255) << 16 |
                  (pixel[1] * alpha / 255) << 8 | (pixel[2] * alpha / 255);
    }
  }
  cairo_surface_mark_dirty(surface);
  g_object_unref(pixbuf);
  return surface;
}

void icon(cairo_t* cr, const Item& item, int x, int y, cairo_surface_t* surface) {
  if (surface) {
    cairo_set_source_surface(cr, surface, x + 4, y + 3);
    cairo_paint(cr);
    return;
  }
  std::error_code ec;
  const bool folder = item.object.kind == ObjectKind::folder ||
    (item.physical && std::filesystem::is_directory(item.path, ec));
  if (item.object.kind == ObjectKind::trash) {
    fill(cr, .25, .25, .29, x + 9, y + 7, 26, 30);
    fill(cr, .66, .66, .68, x + 5, y + 4, 34, 5);
  } else if (folder) {
    fill(cr, .95, .78, .35, x + 5, y + 12, 38, 28);
    fill(cr, .99, .88, .51, x + 5, y + 6, 20, 10);
    draw_bevel(cr, x + 5, y + 12, 38, 28);
  } else {
    fill(cr, .94, .94, .91, x + 10, y + 4, 29, 37);
    draw_bevel(cr, x + 10, y + 4, 29, 37);
    fill(cr, .20, .36, .65, x + 15, y + 13, 18, 3);
    fill(cr, .40, .40, .43, x + 15, y + 22, 17, 2);
    fill(cr, .40, .40, .43, x + 15, y + 28, 14, 2);
  }
  if (item.object.kind == ObjectKind::shadow) {
    fill(cr, .05, .10, .45, x + 28, y + 29, 14, 13);
    draw_text(cr, "↗", x + 29, y + 26, 12, 12, true);
  }
}

xcb_visualtype_t* visual_for(xcb_screen_t* screen) {
  for (auto depth = xcb_screen_allowed_depths_iterator(screen); depth.rem; xcb_depth_next(&depth))
    for (auto visual = xcb_depth_visuals_iterator(depth.data); visual.rem; xcb_visualtype_next(&visual))
      if (visual.data->visual_id == screen->root_visual) return visual.data;
  return nullptr;
}
} // namespace

struct Shell::Impl {
  static constexpr int panel_height = 30;
  xcb_connection_t* connection;
  xcb_screen_t* screen;
  Store& store;
  ShellModel model;
  ThemeConfig config{load_config()};
  void bevel(cairo_t* cr, int x, int y, int w, int h, bool sunken = false) const {
    draw_bevel(cr, x, y, w, h, sunken, &config);
  }

  void text(cairo_t* cr, const std::string& value, int x, int y, int width,
            int size = 11, bool white = false, bool bold = false) const {
    draw_text(cr, value, x, y, width, size, white, bold, config);
  }

  void themed_fill(cairo_t* cr, const std::array<double, 3>& dark,
                   double r, double g, double b, int x, int y, int w, int h) const {
    if (config.dark_mode) fill(cr, dark[0], dark[1], dark[2], x, y, w, h);
    else fill(cr, r, g, b, x, y, w, h);
  }

  VolumeControl volume;
  std::optional<VolumeState> volume_state;
  std::chrono::steady_clock::time_point last_volume_poll{};
  std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
  int transfers_in_progress{};
  xcb_visualtype_t* visual{};
  xcb_window_t desktop{}, panel{}, menu{}, submenu{}, app_menu{}, exit_dialog{};
  xcb_window_t notice{};
  std::string notice_text;
  std::optional<Properties> properties;
  std::optional<IconChooser> icon_chooser;
  std::optional<OpenWithDialog> open_with_dialog;
  std::uint16_t width{}, height{};
  std::uint16_t physical_width{}, physical_height{};
  double scale{1.0};
  Wallpaper wallpaper;
  xcb_pixmap_t desktop_pixmap{};
  int wallpaper_width{}, wallpaper_height{};
  std::vector<DisplayMonitor> display_monitors;
  DisplayMonitor primary_monitor{};
  int panel_width{};
  int desktop_width{}, desktop_height{};
  std::unordered_map<xcb_window_t, Folder> folders;
  std::unordered_set<std::string> hidden_shortcuts;
  xcb_window_t focused_folder{};
  std::unordered_map<xcb_window_t, GFileMonitor*> monitors;
  std::vector<Item> desktop_items;
  std::vector<Task> tasks;
  int requested_tray_width{};
  std::vector<GAppInfo*> apps;
  std::vector<AppCategory> app_categories;
  std::vector<std::filesystem::path> icon_roots;
  std::unordered_map<std::string, IconTheme> icon_themes;
  std::unordered_map<std::string, std::filesystem::path> icon_files;
  std::unordered_map<std::string, cairo_surface_t*> icon_surfaces;
  std::unordered_map<std::string, std::string> file_content_types;
  std::unordered_map<std::string, cairo_surface_t*> file_icon_cache;
  ThumbnailCache thumbnail_cache;
  StartupNotification* startup{};
  cairo_surface_t* logo_surface{};
  cairo_surface_t* exit_logo_surface{};
  std::string icon_theme{selected_icon_theme()};
  std::vector<MenuEntry> menu_entries;
  std::vector<MenuEntry> submenu_entries;
  std::vector<MenuEntry> app_menu_entries;
  std::vector<RecentFile> recent_menu_files;
  std::size_t category_page{};
  std::size_t selected_category{};
  std::size_t app_page{};
  std::size_t task_page{};
  xcb_window_t menu_owner{};
  int menu_x{}, menu_y{}, menu_width{250};
  int submenu_x{}, submenu_y{}, category_row{};
  int menu_hover{-1};
  int submenu_hover{-1};
  int app_menu_hover{-1};
  int selected_desktop{-1};
  int desktop_scroll_rows{};
  std::optional<Item> selected_item;
  std::filesystem::path clipboard_path;
  bool clipboard_cut{};
  std::function<void(const std::vector<std::filesystem::path>&, bool)> clipboard_set;
  std::function<void(std::function<void(std::vector<std::filesystem::path>, bool)>)> clipboard_request;
  std::function<void(xcb_window_t, Shell::DropHandler)> dnd_register;
  std::function<void(xcb_window_t)> dnd_unregister;
  std::function<void(xcb_window_t, std::vector<std::filesystem::path>, bool)> dnd_begin;
  std::function<void()> exit_handler;
  std::string settings_program{"midnightbsd-settings"};
  xcb_key_symbols_t* key_symbols{};
  xcb_atom_t active_atom{};
  std::string last_clock;
  xcb_timestamp_t last_click_time{};
  int last_click_index{-1};
  xcb_window_t last_click_window{};
  xcb_window_t drag_window{};
  xcb_window_t scroll_drag_window{};
  int scroll_grab_offset{};
  int drag_root_x{}, drag_root_y{}, drag_start_x{}, drag_start_y{};
  bool resizing{};
  xcb_window_t pending_icon_window{};
  int pending_icon_index{-1};
  int pending_icon_x{}, pending_icon_y{};

  Impl(xcb_connection_t* c, xcb_screen_t* s, Store& st) : connection(c), screen(s), store(st), model(st),
      physical_width(s->width_in_pixels), physical_height(s->height_in_pixels) {
    update_monitor_geometry();
    scale = display_scale(primary_monitor.height, config.ui_scale);
    width = std::max(1, static_cast<int>(physical_width / scale));
    height = std::max(1, static_cast<int>(physical_height / scale));
    update_monitor_geometry();
  }

  void update_monitor_geometry() {
    display_monitors = query_display_monitors(connection, screen);
    if (display_monitors.empty())
      display_monitors.push_back({0, 0, physical_width, physical_height, true});
    // A synthetic root ConfigureNotify used by isolated resize checks cannot
    // update RandR's sole monitor. The root event still supplies the new bounds.
    if (display_monitors.size() == 1 && display_monitors.front().x == 0 &&
        display_monitors.front().y == 0 &&
        (display_monitors.front().width != physical_width ||
         display_monitors.front().height != physical_height)) {
      display_monitors.front().width = physical_width;
      display_monitors.front().height = physical_height;
    }
    const auto found = std::ranges::find_if(display_monitors, [](const auto& monitor) {
      return monitor.primary;
    });
    primary_monitor = found == display_monitors.end() ? display_monitors.front() : *found;
    panel_width = std::max(1, static_cast<int>(primary_monitor.width / scale));
    desktop_width = panel_width;
    desktop_height = std::max(1, static_cast<int>(primary_monitor.height / scale) - panel_height);
    menu_width = launcher_menu_width(panel_width);
  }

  struct MonitorArea { int x{}, y{}, width{}, height{}; };

  MonitorArea monitor_area(const DisplayMonitor& monitor) const {
    return {static_cast<int>(std::lround(monitor.x / scale)),
            static_cast<int>(std::lround(monitor.y / scale)),
            std::max(1, static_cast<int>(monitor.width / scale)),
            std::max(1, static_cast<int>(monitor.height / scale))};
  }

  MonitorArea primary_area() const { return monitor_area(primary_monitor); }

  MonitorArea area_at(int x, int y) const {
    return monitor_area(choose_display_monitor(display_monitors,
      static_cast<int>(std::lround(x * scale)), static_cast<int>(std::lround(y * scale))));
  }

  MonitorArea pointer_area() const {
    const auto cookie = xcb_query_pointer(connection, screen->root);
    auto* reply = xcb_query_pointer_reply(connection, cookie, nullptr);
    if (!reply) return primary_area();
    const auto area = monitor_area(choose_display_monitor(display_monitors, reply->root_x, reply->root_y));
    std::free(reply);
    return area;
  }

  MonitorArea owner_area(xcb_window_t owner) const {
    if (auto it = folders.find(owner); it != folders.end())
      return area_at(it->second.x + it->second.width / 2,
                     it->second.y + it->second.height / 2);
    if (owner == panel) return primary_area();
    if (owner == desktop) return pointer_area();
    return pointer_area();
  }

  std::pair<int, int> centered(const MonitorArea& area, int w, int h) const {
    const auto primary = primary_area();
    const auto available_height = [&primary](const MonitorArea& target) {
      return target.height - (target.x == primary.x && target.y == primary.y ? panel_height : 0);
    };
    const auto target = (w > area.width || h > available_height(area)) &&
                        w <= primary.width && h <= available_height(primary) ? primary : area;
    const int top = target.y + (target.x == primary.x && target.y == primary.y ? panel_height : 0);
    return {std::clamp(target.x + (target.width - w) / 2, target.x,
                       std::max(target.x, target.x + target.width - w)),
            std::clamp(target.y + (target.height - h) / 2, top,
                       std::max(top, target.y + target.height - h))};
  }
  ~Impl() {
    *alive = false;
    if (key_symbols) xcb_key_symbols_free(key_symbols);
    for (auto* app : apps) g_object_unref(app);
    for (auto& [name, surface] : icon_surfaces) if (surface) cairo_surface_destroy(surface);
    if (logo_surface) cairo_surface_destroy(logo_surface);
    if (exit_logo_surface) cairo_surface_destroy(exit_logo_surface);
    for (auto& [window, monitor] : monitors) g_object_unref(monitor);
    for (auto& [window, folder] : folders) xcb_destroy_window(connection, window);
    if (menu) xcb_destroy_window(connection, menu);
    if (submenu) xcb_destroy_window(connection, submenu);
    if (app_menu) xcb_destroy_window(connection, app_menu);
    if (exit_dialog) xcb_destroy_window(connection, exit_dialog);
    if (notice) xcb_destroy_window(connection, notice);
    if (icon_chooser) xcb_destroy_window(connection, icon_chooser->window);
    if (open_with_dialog) {
      for (auto* app : open_with_dialog->apps) g_object_unref(app);
      if (open_with_dialog->default_app) g_object_unref(open_with_dialog->default_app);
      xcb_destroy_window(connection, open_with_dialog->window);
    }
    if (properties) xcb_destroy_window(connection, properties->window);
    if (panel) xcb_destroy_window(connection, panel);
    if (desktop) xcb_destroy_window(connection, desktop);
    if (desktop_pixmap) xcb_free_pixmap(connection, desktop_pixmap);
  }

  xcb_window_t create_window(int x, int y, int w, int h, std::uint32_t extra_mask = 0) {
    auto id = xcb_generate_id(connection);
    const std::uint32_t values[] = {screen->black_pixel, 1,
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
      XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY | extra_mask};
    xcb_create_window(connection, screen->root_depth, id, screen->root,
                      static_cast<std::int16_t>(std::lround(x * scale)),
                      static_cast<std::int16_t>(std::lround(y * scale)),
                      scaled_pixel(scale, w), scaled_pixel(scale, h), 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
    return id;
  }

  void paint(xcb_window_t window, int w, int h, auto&& draw) {
    if (!visual || w <= 0 || h <= 0) return;
    auto* surface = cairo_xcb_surface_create(connection, window, visual,
                                             scaled_pixel(scale, w), scaled_pixel(scale, h));
    auto* cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    draw(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    xcb_flush(connection);
  }

  void paint_desktop(auto&& draw) {
    if (!visual || !desktop || !physical_width || !physical_height) return;
    // Build a complete replacement before publishing it. Drawing into the
    // currently installed background could expose a partly painted image.
    const auto pixmap = xcb_generate_id(connection);
    auto* error = xcb_request_check(connection, xcb_create_pixmap_checked(
        connection, screen->root_depth, pixmap, desktop, physical_width, physical_height));
    if (error) {
      std::free(error);
      return; // Keep the previous complete background if allocation fails.
    }
    auto* surface = cairo_xcb_surface_create(connection, pixmap, visual,
                                             physical_width, physical_height);
    auto* cr = cairo_create(surface);
    // Cover the exact physical extent, including fractional-scale edge pixels.
    cairo_set_source_rgb(cr, config.desktop_color[0], config.desktop_color[1], config.desktop_color[2]);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    draw(cr);
    const auto status = cairo_status(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    const bool complete = status == CAIRO_STATUS_SUCCESS &&
                          cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(surface);
    if (!complete) {
      xcb_free_pixmap(connection, pixmap);
      return;
    }
    xcb_change_window_attributes(connection, desktop, XCB_CW_BACK_PIXMAP, &pixmap);
    // Clear copies the finished background in one server operation. Xorg also
    // uses it automatically when dragging another window uncovers the desktop.
    xcb_clear_area(connection, false, desktop, 0, 0, 0, 0);
    if (desktop_pixmap) xcb_free_pixmap(connection, desktop_pixmap);
    desktop_pixmap = pixmap;
    xcb_flush(connection);
  }

  void reload_apps() {
    for (auto* app : apps) g_object_unref(app);
    apps.clear();
    app_categories.clear();
    std::filesystem::path menu_file;
    const char* prefix = g_getenv("XDG_MENU_PREFIX");
    const std::string menu_name = std::string(prefix ? prefix : "") + "applications.menu";
    for (const char* const* dir = g_get_system_config_dirs(); dir && *dir; ++dir) {
      const auto candidate = std::filesystem::path(*dir) / "menus" / menu_name;
      if (std::filesystem::is_regular_file(candidate)) { menu_file = candidate; break; }
    }
    const auto user_menu = std::filesystem::path(g_get_user_config_dir()) / "menus" / menu_name;
    if (std::filesystem::is_regular_file(user_menu)) menu_file = user_menu;
    if (std::filesystem::is_regular_file(menu_file)) {
      auto* menu = garcon_menu_new_for_path(menu_file.c_str());
      GError* error = nullptr;
      if (menu && garcon_menu_load(menu, nullptr, &error)) {
        const auto collect = [&](auto&& self, GarconMenu* current, const std::string& parent) -> void {
          auto* directory = garcon_menu_get_directory(current);
          const char* menu_label = directory ? garcon_menu_directory_get_name(directory) : nullptr;
          if (!menu_label || !*menu_label) menu_label = garcon_menu_element_get_name(GARCON_MENU_ELEMENT(current));
          const std::string label = parent.empty() ? (menu_label ? menu_label : "Other") :
            parent + " / " + (menu_label ? menu_label : "Other");
          GList* elements = garcon_menu_get_elements(current);
          AppCategory category{label, directory && garcon_menu_directory_get_icon_name(directory) ?
            garcon_menu_directory_get_icon_name(directory) : "applications-other", {}};
          for (GList* node = elements; node; node = node->next) {
            auto* element = GARCON_MENU_ELEMENT(node->data);
            if (!garcon_menu_element_get_visible(element)) continue;
            if (GARCON_IS_MENU_ITEM(element)) {
              auto* file = garcon_menu_item_get_file(GARCON_MENU_ITEM(element));
              char* path = file ? g_file_get_path(file) : nullptr;
              auto* app = path ? g_desktop_app_info_new_from_filename(path) : nullptr;
              g_free(path);
              if (app && g_app_info_should_show(G_APP_INFO(app))) {
                category.apps.push_back(apps.size());
                apps.push_back(G_APP_INFO(app));
              } else if (app) g_object_unref(app);
            } else if (GARCON_IS_MENU(element)) {
              self(self, GARCON_MENU(element), label);
            }
          }
          g_list_free(elements);
          if (!category.apps.empty()) app_categories.push_back(std::move(category));
        };
        GList* root_elements = garcon_menu_get_elements(menu);
        for (GList* node = root_elements; node; node = node->next)
          if (GARCON_IS_MENU(node->data) && garcon_menu_element_get_visible(GARCON_MENU_ELEMENT(node->data)))
            collect(collect, GARCON_MENU(node->data), "");
        g_list_free(root_elements);
      }
      if (error) g_error_free(error);
      if (menu) g_object_unref(menu);
      if (!app_categories.empty()) return;
    }
    GList* list = g_app_info_get_all();
    for (GList* cursor = list; cursor; cursor = cursor->next) {
      auto* app = G_APP_INFO(cursor->data);
      if (g_app_info_should_show(app)) apps.push_back(G_APP_INFO(g_object_ref(app)));
    }
    g_list_free_full(list, g_object_unref);
    std::ranges::sort(apps, [](GAppInfo* a, GAppInfo* b) {
      return g_utf8_collate(g_app_info_get_display_name(a), g_app_info_get_display_name(b)) < 0;
    });
    std::array<std::vector<std::size_t>, app_category_definitions.size()> grouped;
    std::vector<std::size_t> other;
    for (std::size_t i = 0; i < apps.size(); ++i) {
      const char* raw = G_IS_DESKTOP_APP_INFO(apps[i]) ?
        g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(apps[i])) : nullptr;
      const std::string_view tags = raw ? raw : "";
      bool matched = false;
      for (std::size_t category = 0; category < app_category_definitions.size(); ++category) {
        const auto key = app_category_definitions[category].key;
        const bool in_category = has_app_category(tags, key) ||
          (key == "AudioVideo" && (has_app_category(tags, "Audio") ||
                                   has_app_category(tags, "Video")));
        if (in_category) {
          grouped[category].push_back(i);
          matched = true;
        }
      }
      if (!matched) other.push_back(i);
    }
    app_categories.clear();
    for (std::size_t category = 0; category < app_category_definitions.size(); ++category) {
      if (grouped[category].empty()) continue;
      const auto& definition = app_category_definitions[category];
      app_categories.push_back({std::string(definition.label), std::string(definition.icon),
                                std::move(grouped[category])});
    }
    if (!other.empty()) app_categories.push_back({"Other", "applications-other", std::move(other)});
  }

  void init_icon_roots() {
    if (!icon_roots.empty()) return;
    icon_roots.emplace_back(std::filesystem::path(g_get_home_dir()) / ".icons");
    icon_roots.emplace_back(std::filesystem::path(g_get_user_data_dir()) / "icons");
    for (const char* const* dir = g_get_system_data_dirs(); dir && *dir; ++dir)
      icon_roots.emplace_back(std::filesystem::path(*dir) / "icons");
  }

  const IconTheme& theme_metadata(const std::string& name) {
    if (auto it = icon_themes.find(name); it != icon_themes.end()) return it->second;
    init_icon_roots();
    IconTheme theme;
    for (const auto& root : icon_roots) {
      auto* file = g_key_file_new();
      const auto index = root / name / "index.theme";
      if (!g_key_file_load_from_file(file, index.c_str(), G_KEY_FILE_NONE, nullptr)) {
        g_key_file_unref(file);
        continue;
      }
      theme.inherits = icon_theme_list(file, "Icon Theme", "Inherits");
      auto directories = icon_theme_list(file, "Icon Theme", "Directories");
      auto scaled = icon_theme_list(file, "Icon Theme", "ScaledDirectories");
      directories.insert(directories.end(), scaled.begin(), scaled.end());
      for (const auto& entry : directories) {
        IconDirectory directory;
        directory.relative = entry;
        if (g_key_file_has_key(file, entry.c_str(), "Size", nullptr))
          directory.size = g_key_file_get_integer(file, entry.c_str(), "Size", nullptr);
        directory.min_size = directory.max_size = directory.size;
        if (g_key_file_has_key(file, entry.c_str(), "MinSize", nullptr))
          directory.min_size = g_key_file_get_integer(file, entry.c_str(), "MinSize", nullptr);
        if (g_key_file_has_key(file, entry.c_str(), "MaxSize", nullptr))
          directory.max_size = g_key_file_get_integer(file, entry.c_str(), "MaxSize", nullptr);
        if (g_key_file_has_key(file, entry.c_str(), "Threshold", nullptr))
          directory.threshold = g_key_file_get_integer(file, entry.c_str(), "Threshold", nullptr);
        if (g_key_file_has_key(file, entry.c_str(), "Scale", nullptr))
          directory.scale = std::max(1, g_key_file_get_integer(file, entry.c_str(), "Scale", nullptr));
        char* type = g_key_file_get_string(file, entry.c_str(), "Type", nullptr);
        if (type) { directory.type = type; g_free(type); }
        theme.directories.push_back(std::move(directory));
      }
      g_key_file_unref(file);
      break;
    }
    return icon_themes.emplace(name, std::move(theme)).first->second;
  }

  std::filesystem::path lookup_theme_icon(const std::string& theme_name, const std::string& icon_name,
                                          std::unordered_set<std::string>& visited) {
    if (!visited.insert(theme_name).second) return {};
    const auto& theme = theme_metadata(theme_name);
    std::filesystem::path best;
    int best_distance = std::numeric_limits<int>::max();
    std::error_code ec;
    for (const auto& root : icon_roots) {
      for (const auto& directory : theme.directories) {
        const int distance = directory.distance(launcher_icon_size);
        if (distance > best_distance) continue;
        for (const char* extension : {".png", ".svg", ".xpm"}) {
          const auto path = root / theme_name / directory.relative / (icon_name + extension);
          if (!std::filesystem::is_regular_file(path, ec)) { ec.clear(); continue; }
          if (distance < best_distance) { best = path; best_distance = distance; }
          break;
        }
      }
    }
    if (!best.empty()) return best;
    for (const auto& inherited : theme.inherits)
      if (auto path = lookup_theme_icon(inherited, icon_name, visited); !path.empty()) return path;
    return {};
  }

  std::filesystem::path themed_icon_path(const std::string& name) {
    if (auto it = icon_files.find(name); it != icon_files.end()) return it->second;
    if (name.empty() || name.find('/') != std::string::npos) return {};
    std::unordered_set<std::string> visited;
    auto path = lookup_theme_icon(icon_theme, name, visited);
    if (path.empty()) path = lookup_theme_icon("hicolor", name, visited);
    if (path.empty()) {
      for (const auto& root : icon_roots) {
        for (const char* extension : {".png", ".svg", ".xpm"}) {
          const auto candidate = root.parent_path() / "pixmaps" / (name + extension);
          std::error_code ec;
          if (std::filesystem::is_regular_file(candidate, ec)) { path = candidate; break; }
        }
        if (!path.empty()) break;
      }
    }
    return icon_files.emplace(name, std::move(path)).first->second;
  }

  std::filesystem::path icon_path(GIcon* icon) {
    if (!icon) return {};
    if (G_IS_EMBLEMED_ICON(icon)) return icon_path(g_emblemed_icon_get_icon(G_EMBLEMED_ICON(icon)));
    if (G_IS_FILE_ICON(icon)) {
      char* filename = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon)));
      std::filesystem::path path = filename ? filename : "";
      g_free(filename);
      return path;
    }
    if (!G_IS_THEMED_ICON(icon)) return {};
    for (const char* const* name = g_themed_icon_get_names(G_THEMED_ICON(icon)); name && *name; ++name)
      if (auto path = themed_icon_path(*name); !path.empty()) return path;
    return {};
  }

  cairo_surface_t* gio_icon_surface(GIcon* icon, int size) {
    if (!icon) return nullptr;
    char* key_text = g_icon_to_string(icon);
    const std::string key = "gio:" + std::to_string(size) + ":" + (key_text ? key_text : "");
    g_free(key_text);
    if (auto it = icon_surfaces.find(key); it != icon_surfaces.end()) return it->second;
    const auto path = icon_path(icon);
    auto* surface = path.empty() ? nullptr : image_surface(path, size);
    icon_surfaces.emplace(key, surface);
    return surface;
  }

  cairo_surface_t* app_icon(GAppInfo* app) {
    return gio_icon_surface(g_app_info_get_icon(app), launcher_icon_size);
  }

  cairo_surface_t* themed_icon_surface(const std::string& name) {
    const std::string key = "theme:" + name;
    if (auto it = icon_surfaces.find(key); it != icon_surfaces.end()) return it->second;
    const auto path = themed_icon_path(name);
    auto* surface = path.empty() ? nullptr : image_surface(path, launcher_icon_size);
    icon_surfaces.emplace(key, surface);
    return surface;
  }

  cairo_surface_t* object_icon_surface(const std::string& name) {
    if (name.empty()) return nullptr;
    const std::string key = "object:" + name;
    if (auto it = icon_surfaces.find(key); it != icon_surfaces.end()) return it->second;
    const auto path = std::filesystem::path(name).is_absolute() ?
      std::filesystem::path(name) : themed_icon_path(name);
    auto* surface = path.empty() ? nullptr : image_surface(path, 40);
    icon_surfaces.emplace(key, surface);
    return surface;
  }

  std::string content_type_for_file(const std::filesystem::path& path) {
    const auto key = path.string();
    if (auto it = file_content_types.find(key); it != file_content_types.end()) return it->second;
    auto* file = g_file_new_for_path(path.c_str());
    auto* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                   G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    std::string type;
    if (info) {
      if (const char* value = g_file_info_get_content_type(info)) type = value;
      g_object_unref(info);
    }
    g_object_unref(file);
    if (type.empty()) {
      gboolean uncertain = FALSE;
      char* guessed = g_content_type_guess(key.c_str(), nullptr, 0, &uncertain);
      if (guessed) { type = guessed; g_free(guessed); }
    }
    return file_content_types.emplace(key, std::move(type)).first->second;
  }

  std::optional<DesktopLauncherInfo> trusted_launcher(const Item& item) const {
    if (!item.physical || item.path.extension() != ".desktop") return std::nullopt;
    auto info = desktop_launcher_info(item.path, Store::desktop_path());
    if (!info || !info->trusted) return std::nullopt;
    return *info;
  }

  cairo_surface_t* thumbnail_surface(const std::filesystem::path& path) {
    const auto thumbnail = thumbnail_cache.thumbnail_for(path);
    if (!thumbnail) return nullptr;
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(*thumbnail, error);
    if (error) return nullptr;
    const auto key = "thumbnail:" + thumbnail->string() + ":" +
                     std::to_string(static_cast<long long>(modified.time_since_epoch().count()));
    if (auto it = icon_surfaces.find(key); it != icon_surfaces.end()) return it->second;
    auto* surface = image_surface(*thumbnail, 40);
    icon_surfaces.emplace(key, surface);
    return surface;
  }

  cairo_surface_t* item_icon_surface(const Item& item, const DesktopLauncherInfo* launcher = nullptr) {
    if (!item.object.icon.empty()) return object_icon_surface(item.object.icon);
    if (launcher && !launcher->icon.empty()) return object_icon_surface(launcher->icon);
    if (!item.physical || item.path.empty()) return nullptr;
    if (auto* surface = thumbnail_surface(item.path)) return surface;
    const auto key = item.path.string();
    if (auto it = file_icon_cache.find(key); it != file_icon_cache.end()) return it->second;
    const auto type = content_type_for_file(item.path);
    GIcon* icon = type.empty() ? nullptr : g_content_type_get_icon(type.c_str());
    auto* surface = gio_icon_surface(icon, 40);
    if (icon) g_object_unref(icon);
    file_icon_cache.emplace(key, surface);
    return surface;
  }

  void collect_theme_icon_names(const std::string& name,
                                std::unordered_set<std::string>& visited,
                                std::unordered_set<std::string>& names) {
    if (!visited.insert(name).second) return;
    const auto& theme = theme_metadata(name);
    for (const auto& root : icon_roots) {
      for (const auto& directory : theme.directories) {
        std::error_code error;
        for (std::filesystem::directory_iterator it(root / name / directory.relative, error), end;
             !error && it != end; it.increment(error)) {
          if (!it->is_regular_file(error)) { error.clear(); continue; }
          const auto extension = it->path().extension();
          if (extension == ".png" || extension == ".svg" || extension == ".xpm")
            names.insert(it->path().stem().string());
        }
      }
    }
    for (const auto& inherited : theme.inherits)
      collect_theme_icon_names(inherited, visited, names);
  }

  std::vector<std::string> available_icon_names() {
    std::unordered_set<std::string> names;
    std::unordered_set<std::string> visited;
    collect_theme_icon_names(icon_theme, visited, names);
    collect_theme_icon_names("hicolor", visited, names);
    for (const auto& root : icon_roots) {
      std::error_code error;
      for (std::filesystem::directory_iterator it(root.parent_path() / "pixmaps", error), end;
           !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) { error.clear(); continue; }
        const auto extension = it->path().extension();
        if (extension == ".png" || extension == ".svg" || extension == ".xpm")
          names.insert(it->path().stem().string());
      }
    }
    std::vector<std::string> result;
    constexpr std::array<std::string_view, 15> common{
      "folder", "user-home", "folder-documents", "folder-download", "folder-pictures",
      "folder-music", "folder-videos", "user-trash", "text-x-generic",
      "application-x-executable", "applications-system", "applications-internet",
      "preferences-system", "drive-harddisk", "computer"};
    for (auto favorite : common)
      if (names.erase(std::string(favorite))) result.emplace_back(favorite);
    std::vector<std::string> remaining(names.begin(), names.end());
    std::ranges::sort(remaining);
    result.insert(result.end(), std::make_move_iterator(remaining.begin()),
                  std::make_move_iterator(remaining.end()));
    return result;
  }

  void load_logo() {
    const std::array<std::filesystem::path, 4> candidates{
      std::filesystem::path(MIDNIGHT_WM_DATA_DIR) / "MidnightBSDLogo.svg",
      "data/MidnightBSDLogo.svg",
      std::filesystem::path(MIDNIGHT_WM_DATA_DIR) / "MidnightBSDLogo22x22.png",
      "data/MidnightBSDLogo22x22.png"
    };
    for (const auto& path : candidates)
      if ((logo_surface = image_surface(path, 20))) {
        exit_logo_surface = image_surface(path, 48);
        break;
      }
  }

  void seed_objects() {
    const auto home = std::filesystem::path(g_get_home_dir());
    auto ensure = [&](std::string id, ObjectKind kind, std::string title, std::string target,
                      int x, int y, std::string icon = {}) {
      if (store.get(id)) return;
      (void)store.put({std::move(id), kind, std::move(title), std::move(target),
                       std::move(icon), "", x, y});
    };
    ensure("home", ObjectKind::folder, "Home", home.string(), 12, 15);
    auto add_user_directory = [&](const char* id, const char* title, GUserDirectory directory,
                                  const char* fallback, const char* icon, int x, int y,
                                  bool enabled = true) {
      const char* configured = g_get_user_special_dir(directory);
      const auto path = configured ? std::filesystem::path(configured) : home / fallback;
      if (!enabled || path == home) { hidden_shortcuts.insert(id); return; }
      if (std::string_view(id) == "templates") {
        if (auto existing = store.get(id); existing &&
            existing->target == (home / "Templates").string() && existing->target != path.string()) {
          existing->target = path.string();
          (void)store.put(*existing);
        }
      }
      ensure(id, ObjectKind::folder, title, path.string(), x, y, icon);
    };
    add_user_directory("downloads", "Downloads", G_USER_DIRECTORY_DOWNLOAD,
                       "Downloads", "folder-download", 102, 15);
    add_user_directory("documents", "Documents", G_USER_DIRECTORY_DOCUMENTS,
                       "Documents", "folder-documents", 192, 15, config.show_documents);
    add_user_directory("pictures", "Pictures", G_USER_DIRECTORY_PICTURES,
                       "Pictures", "folder-pictures", 282, 15, config.show_pictures);
    add_user_directory("music", "Music", G_USER_DIRECTORY_MUSIC,
                       "Music", "folder-music", 372, 15, config.show_music);
    add_user_directory("videos", "Videos", G_USER_DIRECTORY_VIDEOS,
                       "Videos", "folder-videos", 462, 15, config.show_videos);
    add_user_directory("public_share", "Public Share", G_USER_DIRECTORY_PUBLIC_SHARE,
                       "Public", "folder-publicshare", 552, 15, config.show_public_share);
    ensure("trash", ObjectKind::trash, "Trash", "trash:///", 12, 112);
    add_user_directory("templates", "Templates", G_USER_DIRECTORY_TEMPLATES,
                       "Templates", "folder-templates", 12, 209);
  }

  std::vector<Item> list_items(const std::filesystem::path& path, bool is_desktop) {
    auto items = model.items(path, is_desktop);
    if (is_desktop)
      std::erase_if(items, [&](const Item& item) { return hidden_shortcuts.contains(item.object.id); });
    return items;
  }

  void sort_folder(Folder& folder) {
    if (!folder.sort_by_type) return;
    std::ranges::stable_sort(folder.items, [](const Item& a, const Item& b) {
      const auto category = [](const Item& item) {
        return item.object.kind == ObjectKind::folder ? 0 :
               item.object.kind == ObjectKind::file ? 1 : 2;
      };
      return category(a) < category(b);
    });
  }

  void refresh() {
    file_content_types.clear();
    file_icon_cache.clear();
    desktop_items = list_items(Store::desktop_path(), true);
    desktop_scroll_rows = std::clamp(desktop_scroll_rows, 0, desktop_max_scroll());
    for (auto& [window, folder] : folders) {
      folder.items = list_items(folder.path, false);
      sort_folder(folder);
      draw_folder(folder);
    }
    draw_desktop();
  }

  void transfer_async(std::vector<std::filesystem::path> paths, std::filesystem::path destination,
                      bool move, std::function<void(std::expected<void, std::string>)> callback) {
    ++transfers_in_progress;
    draw_panel();
    auto* completion = new TransferCompletion{alive, std::move(callback)};
    auto* task = g_task_new(nullptr, nullptr,
      +[](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<TransferCompletion> completion(static_cast<TransferCompletion*>(data));
        std::unique_ptr<std::expected<void, std::string>> value(
          static_cast<std::expected<void, std::string>*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
        if (*completion->alive && value) completion->callback(std::move(*value));
      }, completion);
    g_task_set_task_data(task, new TransferWork{std::move(paths), std::move(destination), move},
                         +[](gpointer data) { delete static_cast<TransferWork*>(data); });
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer data, GCancellable*) {
      const auto* work = static_cast<TransferWork*>(data);
      auto result = ShellModel::transfer(work->paths, work->destination, work->move);
      g_task_return_pointer(task, new std::expected<void, std::string>(std::move(result)),
                            +[](gpointer pointer) { delete static_cast<std::expected<void, std::string>*>(pointer); });
    });
    g_object_unref(task);
  }

  void dropped(xcb_window_t target, std::vector<std::filesystem::path> paths, bool move, Shell::DropFinish finish) {
    const auto folder = folders.find(target);
    const auto destination = target == desktop ? Store::desktop_path() : folder == folders.end() ? std::filesystem::path{} : folder->second.path;
    if (destination.empty()) { finish(false); return; }
    transfer_async(std::move(paths), destination, move,
      [this, finish = std::move(finish)](std::expected<void, std::string> result) mutable {
        --transfers_in_progress;
        if (!result) show_notice("Drop failed: " + result.error());
        else refresh();
        draw_panel();
        finish(result.has_value());
      });
  }

  void register_drop(xcb_window_t window) {
    if (dnd_register) dnd_register(window, [this](xcb_window_t target, std::vector<std::filesystem::path> paths,
                                                  bool move, int, int, Shell::DropFinish finish) {
      dropped(target, std::move(paths), move, std::move(finish));
    });
  }

  void watch_directory(xcb_window_t window, const std::filesystem::path& path) {
    auto* file = path == "trash:///" ? g_file_new_for_uri("trash:///") : g_file_new_for_path(path.c_str());
    GError* error = nullptr;
    auto* monitor = g_file_monitor_directory(file, G_FILE_MONITOR_NONE, nullptr, &error);
    g_object_unref(file);
    if (error) g_error_free(error);
    if (!monitor) return;
    g_signal_connect(monitor, "changed", G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*,
                                                         GFileMonitorEvent, gpointer user_data) {
      static_cast<Impl*>(user_data)->refresh();
    }), this);
    monitors.emplace(window, monitor);
  }

  void unwatch(xcb_window_t window) {
    if (auto it = monitors.find(window); it != monitors.end()) {
      g_object_unref(it->second);
      monitors.erase(it);
    }
  }

  void draw_panel() {
    const auto all_tasks = panel_tasks();
    const auto layout = task_layout(all_tasks.size());
    paint(panel, panel_width, panel_height, [&](cairo_t* cr) {
      fill(cr, config.panel_color[0], config.panel_color[1], config.panel_color[2], 0, 0, panel_width, panel_height);
      fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2], 0, 0, panel_width, 3);
      themed_fill(cr, config.button_color, .85, .85, .85, 4, 4, 116, 22);
      bevel(cr, 4, 4, 116, 22);
      if (logo_surface) {
        cairo_set_source_surface(cr, logo_surface, 7, 5);
        cairo_paint(cr);
      }
      text(cr, config.panel_label, logo_surface ? 31 : 13, 7,
           logo_surface ? 84 : 102, 11, false, true);
      bevel(cr, lock_button_x, 5, lock_button_width, 24);
      lock_icon(cr, lock_button_x + 4, 8, config);
      bevel(cr, terminal_button_x, 5, terminal_button_width, 24);
      terminal_icon(cr, terminal_button_x + 3, 9);
      bevel(cr, browser_button_x, 5, browser_button_width, 24);
      browser_icon(cr, browser_button_x + 4, 9, config);
      const int clock_x = this->clock_x();
      const int volume_x = this->volume_x();
      const bool show_volume = this->show_volume();
      if (transfers_in_progress) text(cr, "Working…", (show_volume ? volume_x : tray_x()) - 85, 8, 80, 11);
      else if (startup && startup->pending_count())
        text(cr, "Starting…", (show_volume ? volume_x : tray_x()) - 85, 8, 80, 11);
      for (std::size_t i = 0; i < layout.visible; ++i) {
        const auto& task = all_tasks[i];
        const int x = task_start_x + static_cast<int>(i) * task_step;
        fill(cr, task.active ? config.accent_color[0] : (config.dark_mode ? config.button_color[0] : .80),
             task.active ? config.accent_color[1] : (config.dark_mode ? config.button_color[1] : .80),
             task.active ? config.accent_color[2] : (config.dark_mode ? config.button_color[2] : .80), x, 5, task_button_width, 24);
        bevel(cr, x, 5, task_button_width, 24, task.active);
        text(cr, task.title, x + 7, 8, 95, 11, task.active);
      }
      if (layout.overflow) {
        bevel(cr, layout.overflow_x, 5, task_overflow_width, 24);
        text(cr, "▾", layout.overflow_x + 6, 6, 16, 15, false, true);
      }
      if (show_volume) {
        bevel(cr, volume_x, 5, 18, 24);
        sound_icon(cr, volume_x + 1, 9, SoundIcon::quieter, config);
        bevel(cr, volume_x + 20, 5, 42, 24, true);
        text(cr, volume_state ? std::to_string(volume_state->percent) + "%" : "N/A",
             volume_x + 23, 8, 36, 11, false, true);
        bevel(cr, volume_x + 64, 5, 18, 24);
        sound_icon(cr, volume_x + 65, 9, SoundIcon::louder, config);
        bevel(cr, volume_x + 84, 5, 18, 24, volume_state && volume_state->muted);
        sound_icon(cr, volume_x + 85, 9, SoundIcon::mute, config);
      }
      bevel(cr, clock_x, 5, 99, 24, true);
      text(cr, clock_text(config.clock_format), clock_x + 7, 8, 86, 11, false, true);
    });
  }

  std::vector<Task> panel_tasks() const {
    auto result = tasks;
    std::vector<xcb_window_t> ids;
    for (const auto& [window, folder] : folders) ids.push_back(window);
    std::ranges::sort(ids);
    for (const auto window : ids) {
      const auto& folder = folders.at(window);
      result.push_back({window, folder.trash ? "Trash" : basename(folder.path),
                        window == focused_folder});
    }
    if (focused_folder)
      for (auto& task : result)
        if (task.window != focused_folder) task.active = false;
    return result;
  }

  int clock_x() const { return std::max(140, panel_width - 105); }

  int tray_width() const {
    // Leave room for the task overflow button even when every task is hidden.
    return std::clamp(requested_tray_width, 0,
                      std::max(0, clock_x() - task_start_x - task_overflow_width - 4));
  }

  int tray_x() const { return clock_x() - tray_width(); }

  int volume_x() const { return tray_x() - volume_group_width - volume_clock_gap; }

  bool show_volume() const { return volume_x() >= 245; }

  TaskLayout task_layout(std::size_t count) const {
    const int limit = (show_volume() ? volume_x() : tray_x()) -
                      (transfers_in_progress ? 85 : 0);
    const auto capacity = [](int right) -> std::size_t {
      return right < task_start_x + task_button_width ? 0 :
        1 + static_cast<std::size_t>((right - task_start_x - task_button_width) / task_step);
    };
    TaskLayout layout;
    layout.visible = std::min(count, capacity(limit));
    layout.overflow = count > layout.visible;
    if (layout.overflow) {
      layout.visible = std::min(count, capacity(limit - task_overflow_width - 4));
      layout.overflow_x = task_start_x + static_cast<int>(layout.visible) * task_step;
    }
    return layout;
  }

  void activate_task_window(xcb_window_t window, xcb_timestamp_t time) {
    const auto all_tasks = panel_tasks();
    if (std::ranges::none_of(all_tasks, [window](const Task& task) { return task.window == window; })) return;
    if (folders.contains(window)) {
      const std::uint32_t above[]{XCB_STACK_MODE_ABOVE};
      xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_STACK_MODE, above);
      xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, window, time);
      raise_panel();
      return;
    }
    xcb_client_message_event_t request{};
    request.response_type = XCB_CLIENT_MESSAGE;
    request.format = 32;
    request.window = window;
    request.type = active_atom;
    request.data.data32[0] = 2;
    request.data.data32[1] = time;
    xcb_send_event(connection, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&request));
    xcb_flush(connection);
  }

  void raise_panel() {
    const std::uint32_t above[]{XCB_STACK_MODE_ABOVE};
    if (panel) xcb_configure_window(connection, panel, XCB_CONFIG_WINDOW_STACK_MODE, above);
    if (menu) xcb_configure_window(connection, menu, XCB_CONFIG_WINDOW_STACK_MODE, above);
    if (submenu) xcb_configure_window(connection, submenu, XCB_CONFIG_WINDOW_STACK_MODE, above);
    if (app_menu) xcb_configure_window(connection, app_menu, XCB_CONFIG_WINDOW_STACK_MODE, above);
    xcb_flush(connection);
  }

  void draw_items(cairo_t* cr, const std::vector<Item>& items, int start_y, int width_px,
                  int selected, bool on_desktop, int scroll_rows = 0) {
    const int cols = std::max(1, width_px / icon_width);
    for (std::size_t i = static_cast<std::size_t>(scroll_rows * cols); i < items.size(); ++i) {
      const int x = 12 + static_cast<int>(i % cols) * icon_width;
      const int y = start_y + (static_cast<int>(i / cols) - scroll_rows) * icon_height;
      if (selected == static_cast<int>(i))
        fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2],
             x, y, 78, 74);
      const auto launcher = trusted_launcher(items[i]);
      icon(cr, items[i], x + 14, y + 3,
           item_icon_surface(items[i], launcher ? &*launcher : nullptr));
      const int label_offset = on_desktop ? 2 : 18;
      text(cr, launcher ? launcher->name : items[i].object.title,
           x + label_offset, y + 50, 78 - label_offset, 10,
           selected == static_cast<int>(i) || on_desktop);
    }
  }

  void draw_desktop() {
    int target_width = 1, target_height = 1;
    for (const auto& monitor : display_monitors) {
      target_width = std::max(target_width, monitor.width);
      target_height = std::max(target_height, monitor.height);
    }
    if (!config.desktop_image.empty() &&
        (target_width != wallpaper_width || target_height != wallpaper_height)) {
      wallpaper_width = target_width;
      wallpaper_height = target_height;
      if (!wallpaper.load(config.desktop_image, target_width, target_height))
        g_warning("Could not load desktop image: %s", config.desktop_image.c_str());
    }
    desktop_scroll_rows = std::clamp(desktop_scroll_rows, 0, desktop_max_scroll());
    paint_desktop([&](cairo_t* cr) {
      fill(cr, config.desktop_color[0], config.desktop_color[1], config.desktop_color[2], 0, 0, width, height);
      for (const auto& monitor : display_monitors)
        wallpaper.paint(cr, monitor.x / scale, monitor.y / scale,
                         monitor.width / scale, monitor.height / scale,
                         config.desktop_image_mode == "fill");
      const auto area = primary_area();
      cairo_save(cr);
      cairo_rectangle(cr, area.x, area.y + panel_height, desktop_width, desktop_height);
      cairo_clip(cr);
      cairo_translate(cr, area.x, area.y + panel_height);
      draw_items(cr, desktop_items, 13, desktop_width, selected_desktop, true, desktop_scroll_rows);
      cairo_restore(cr);
    });
  }

  int desktop_visible_rows() const {
    return std::max(1, (desktop_height - 13) / icon_height);
  }

  int desktop_max_scroll() const {
    const int columns = std::max(1, desktop_width / icon_width);
    const int rows = static_cast<int>((desktop_items.size() + columns - 1) / columns);
    return std::max(0, rows - desktop_visible_rows());
  }

  void draw_folder(Folder& folder) {
    auto scroll = folder_scroll(folder);
    folder.scroll_rows = std::clamp(folder.scroll_rows, 0, scroll.max_scroll);
    scroll = folder_scroll(folder);
    paint(folder.window, folder.width, folder.height, [&](cairo_t* cr) {
      themed_fill(cr, config.surface_color, .73, .73, .73, 0, 0, folder.width, folder.height);
      bevel(cr, 0, 0, folder.width, folder.height);
      fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2],
           4, 4, folder.width - 8, 26);
      text(cr, folder.trash ? "Trash" : basename(folder.path), 12, 7, folder.width - 65, 13, true, true);
      themed_fill(cr, config.surface_color, .73, .73, .73, folder.width - 30, 7, 20, 19);
      bevel(cr, folder.width - 30, 7, 20, 19);
      text(cr, "×", folder.width - 25, 6, 17, 15);
      text(cr, "File", 12, 34, 36, 11);
      text(cr, "Edit", 56, 34, 36, 11);
      text(cr, "View", 102, 34, 42, 11);
      themed_fill(cr, config.field_color, .96, .96, .92, 5, folder_header, scroll.view_width, scroll.view_height);
      cairo_save(cr);
      cairo_rectangle(cr, 5, folder_header, scroll.view_width, scroll.view_height);
      cairo_clip(cr);
      draw_items(cr, folder.items, folder_header + 11, scroll.view_width, folder.selected, false, folder.scroll_rows);
      cairo_restore(cr);
      themed_fill(cr, config.button_color, .69, .69, .69, scroll.x, folder_header, scrollbar_width, scroll.view_height);
      bevel(cr, scroll.x, folder_header, scrollbar_width, scrollbar_width);
      text(cr, "▲", scroll.x + 3, folder_header + 2, 12, 10);
      const int down_y = folder_header + scroll.view_height - scrollbar_width;
      bevel(cr, scroll.x, down_y, scrollbar_width, scrollbar_width);
      text(cr, "▼", scroll.x + 3, down_y + 2, 12, 10);
      themed_fill(cr, config.field_color, .57, .57, .57, scroll.x, scroll.track_y, scrollbar_width, scroll.track_height);
      bevel(cr, scroll.x, scroll.track_y, scrollbar_width, scroll.track_height, true);
      themed_fill(cr, config.button_color, .77, .77, .77, scroll.x + 1, scroll.thumb_y, scrollbar_width - 2, scroll.thumb_height);
      bevel(cr, scroll.x + 1, scroll.thumb_y, scrollbar_width - 2, scroll.thumb_height);
      themed_fill(cr, config.surface_color, .73, .73, .73, 5, folder.height - 21, folder.width - 10, 16);
      text(cr, std::to_string(folder.items.size()) + " objects", 11, folder.height - 21, folder.width - 22, 10);
    });
  }

  void draw_menu_window(xcb_window_t window, const std::vector<MenuEntry>& entries, int hover) {
    if (!window) return;
    const int h = static_cast<int>(entries.size()) * row_height + 8;
    paint(window, menu_width, h, [&](cairo_t* cr) {
      themed_fill(cr, config.surface_color, .79, .79, .79, 0, 0, menu_width, h);
      bevel(cr, 0, 0, menu_width, h);
      themed_fill(cr, config.button_color, .70, .70, .70, 4, 4, 25, h - 8);
      themed_fill(cr, config.shadow_color, .45, .45, .45, 29, 4, 1, h - 8);
      for (std::size_t i = 0; i < entries.size(); ++i) {
        const int y = 4 + static_cast<int>(i) * row_height;
        const bool selected = static_cast<int>(i) == hover;
        if (selected) fill(cr, config.accent_color[0], config.accent_color[1],
                           config.accent_color[2], 4, y, menu_width - 8, row_height);
        const auto& entry = entries[i];
        if (entry.action == MenuAction::launch && entry.index < apps.size()) {
          if (auto* surface = app_icon(apps[entry.index])) {
            cairo_set_source_surface(cr, surface, 8, y + 3);
            cairo_paint(cr);
          }
        } else if (entry.action == MenuAction::open_trash) {
          if (auto* surface = themed_icon_surface("user-trash")) {
            cairo_set_source_surface(cr, surface, 8, y + 3);
            cairo_paint(cr);
          } else {
            fill(cr, .25, .25, .29, 12, y + 8, 12, 13);
            fill(cr, .66, .66, .68, 10, y + 6, 16, 3);
          }
        } else if (entry.action == MenuAction::show_category && entry.index < app_categories.size()) {
          if (auto* surface = themed_icon_surface(app_categories[entry.index].icon)) {
            cairo_set_source_surface(cr, surface, 8, y + 3);
            cairo_paint(cr);
          } else {
            fill(cr, .96, .77, .31, 8, y + 9, 16, 11);
            fill(cr, .99, .88, .51, 8, y + 6, 9, 4);
            draw_bevel(cr, 8, y + 9, 16, 11);
          }
        } else if (entry.action == MenuAction::open_home || entry.action == MenuAction::open_desktop ||
                   entry.action == MenuAction::new_folder || entry.action == MenuAction::show_programs) {
          fill(cr, .96, .77, .31, 8, y + 9, 16, 11);
          fill(cr, .99, .88, .51, 8, y + 6, 9, 4);
          draw_bevel(cr, 8, y + 9, 16, 11);
        } else if (entry.action == MenuAction::exit_session) {
          themed_fill(cr, config.button_color, .94, .94, .89, 9, y + 5, 14, 14);
          bevel(cr, 9, y + 5, 14, 14);
          text(cr, "×", 12, y + 4, 10, 12);
        }
        text(cr, entry.label, 35, y + 4, menu_width - 42, 11, selected);
      }
    });
  }

  void draw_menu() { draw_menu_window(menu, menu_entries, menu_hover); }
  void draw_submenu() { draw_menu_window(submenu, submenu_entries, submenu_hover); }
  void draw_app_menu() { draw_menu_window(app_menu, app_menu_entries, app_menu_hover); }

  void show_notice(std::string message) {
    notice_text = std::move(message);
    if (notice) xcb_destroy_window(connection, notice);
    const auto [x, y] = centered(owner_area(menu_owner), 420, 100);
    notice = create_window(x, y, 420, 100);
    xcb_map_window(connection, notice);
    draw_notice();
  }

  bool can_open_with(const Item& item) const {
    std::error_code error;
    return !item.trashed && !item.path.empty() &&
      std::filesystem::is_regular_file(item.path, error);
  }

  void close_open_with_dialog() {
    if (!open_with_dialog) return;
    const auto owner = open_with_dialog->owner;
    for (auto* app : open_with_dialog->apps) g_object_unref(app);
    if (open_with_dialog->default_app) g_object_unref(open_with_dialog->default_app);
    xcb_destroy_window(connection, open_with_dialog->window);
    open_with_dialog.reset();
    if (owner == desktop || folders.contains(owner))
      xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, owner, XCB_CURRENT_TIME);
  }

  void draw_open_with_dialog() {
    if (!open_with_dialog) return;
    const auto& dialog = *open_with_dialog;
    paint(dialog.window, open_with_width, open_with_height, [&](cairo_t* cr) {
      themed_fill(cr, config.surface_color, .75, .75, .75, 0, 0, open_with_width, open_with_height);
      bevel(cr, 0, 0, open_with_width, open_with_height);
      fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2],
           4, 4, open_with_width - 8, 26);
      text(cr, "Open With", 12, 7, open_with_width - 24, 12, true, true);
      text(cr, basename(dialog.path), 12, 36, open_with_width - 24, 11);
      themed_fill(cr, config.field_color, .96, .96, .92, 10, 56, open_with_width - 20, 282);
      bevel(cr, 10, 56, open_with_width - 20, 282, true);
      for (std::size_t row = 0; row < open_with_rows && row + dialog.scroll < dialog.apps.size(); ++row) {
        const auto index = row + dialog.scroll;
        auto* app = dialog.apps[index];
        const int y = 57 + static_cast<int>(row) * 28;
        const bool selected = index == dialog.selected;
        if (selected)
          fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2],
               12, y, open_with_width - 24, 27);
        if (auto* surface = app_icon(app)) {
          cairo_set_source_surface(cr, surface, 17, y + 4);
          cairo_paint(cr);
        }
        const bool is_default = dialog.default_app && g_app_info_equal(app, dialog.default_app);
        const std::string label = std::string(g_app_info_get_display_name(app)) +
          (is_default ? " (default)" : "");
        text(cr, label, 42, y + 5, open_with_width - 58, 11, selected);
      }
      if (dialog.apps.size() > open_with_rows)
        text(cr, std::to_string(dialog.scroll + 1) + "–" +
                 std::to_string(std::min(dialog.apps.size(), dialog.scroll + open_with_rows)) +
                 " of " + std::to_string(dialog.apps.size()), 355, 339, 90, 10);
      for (const auto [x, w, label] : {std::tuple{10, 100, "Open"},
                                      std::tuple{118, 190, "Set Default & Open"},
                                      std::tuple{316, 132, "Cancel"}}) {
        bevel(cr, x, 355, w, 26);
        text(cr, label, x + 8, 359, w - 12, 11);
      }
    });
  }

  void show_open_with_dialog(const Item& item) {
    if (!can_open_with(item)) return;
    close_open_with_dialog();
    OpenWithDialog dialog;
    dialog.owner = menu_owner;
    dialog.path = item.path;
    dialog.content_type = content_type_for_file(item.path);
    if (dialog.content_type.empty()) { show_notice("Could not determine file type"); return; }
    dialog.default_app = g_app_info_get_default_for_type(dialog.content_type.c_str(), FALSE);
    const auto add_app = [&](GAppInfo* app) {
      if (!app || (!g_app_info_supports_files(app) && !g_app_info_supports_uris(app))) return;
      if (std::ranges::any_of(dialog.apps, [app](GAppInfo* existing) {
            return g_app_info_equal(existing, app);
          })) return;
      dialog.apps.push_back(G_APP_INFO(g_object_ref(app)));
    };
    add_app(dialog.default_app);
    GList* recommended = g_app_info_get_recommended_for_type(dialog.content_type.c_str());
    for (GList* node = recommended; node; node = node->next) add_app(G_APP_INFO(node->data));
    g_list_free_full(recommended, g_object_unref);
    const auto preferred_count = dialog.apps.size();
    GList* all = g_app_info_get_all();
    for (GList* node = all; node; node = node->next) {
      auto* app = G_APP_INFO(node->data);
      if (g_app_info_should_show(app)) add_app(app);
    }
    g_list_free_full(all, g_object_unref);
    std::ranges::sort(dialog.apps.begin() + static_cast<std::ptrdiff_t>(preferred_count),
                      dialog.apps.end(), [](GAppInfo* a, GAppInfo* b) {
      return g_utf8_collate(g_app_info_get_display_name(a),
                            g_app_info_get_display_name(b)) < 0;
    });
    if (dialog.apps.empty()) {
      if (dialog.default_app) g_object_unref(dialog.default_app);
      show_notice("No application can open this file");
      return;
    }
    const auto [x, y] = centered(owner_area(dialog.owner), open_with_width, open_with_height);
    dialog.window = create_window(x, y, open_with_width, open_with_height);
    open_with_dialog = std::move(dialog);
    xcb_map_window(connection, open_with_dialog->window);
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT,
                        open_with_dialog->window, XCB_CURRENT_TIME);
    draw_open_with_dialog();
  }

  void accept_open_with(bool set_default) {
    if (!open_with_dialog || open_with_dialog->selected >= open_with_dialog->apps.size()) return;
    auto& dialog = *open_with_dialog;
    auto* app = dialog.apps[dialog.selected];
    GError* error = nullptr;
    if (set_default && !g_app_info_set_as_default_for_type(app, dialog.content_type.c_str(), &error)) {
      show_notice(std::string("Could not set default: ") + (error ? error->message : "unknown error"));
      if (error) g_error_free(error);
      return;
    }
    auto* file = g_file_new_for_path(dialog.path.c_str());
    GList* files = g_list_append(nullptr, file);
    const gboolean launched = startup ? startup->launch(app, files, &error) :
      g_app_info_launch(app, files, nullptr, &error);
    if (launched) remember_file(dialog.path);
    g_list_free(files);
    g_object_unref(file);
    close_open_with_dialog();
    if (!launched) {
      show_notice(std::string("Open failed: ") + (error ? error->message : "unknown error"));
    }
    if (error) g_error_free(error);
  }

  void show_properties(const Item& item) {
    if (icon_chooser) close_icon_chooser();
    if (properties) xcb_destroy_window(connection, properties->window);
    Properties dialog;
    dialog.item = item;
    dialog.owner = menu_owner;
    dialog.values = {item.object.title, item.object.icon, item.object.target};
    const auto [x, y] = centered(owner_area(dialog.owner), 500, 220);
    dialog.window = create_window(x, y, 500, 220);
    properties = std::move(dialog);
    xcb_map_window(connection, properties->window);
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, properties->window, XCB_CURRENT_TIME);
    draw_properties();
  }

  void draw_properties() {
    if (!properties) return;
    paint(properties->window, 500, 220, [&](cairo_t* cr) {
      themed_fill(cr, config.surface_color, .75, .75, .75, 0, 0, 500, 220);
      bevel(cr, 0, 0, 500, 220);
      fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2],
           4, 4, 492, 27);
      text(cr, "Properties", 13, 8, 450, 12, true, true);
      constexpr std::array<std::string_view, 3> labels{"Name", "Icon", "Target"};
      for (int i = 0; i < 3; ++i) {
        const int y = 47 + i * 45;
        text(cr, std::string(labels[i]), 14, y + 5, 80, 11);
        themed_fill(cr, config.field_color, 1, 1, 1, 92, y, 390, 29);
        bevel(cr, 92, y, 390, 29, true);
        if (i == 1) {
          if (auto* surface = object_icon_surface(properties->values[i])) {
            cairo_save(cr);
            cairo_translate(cr, 97, y + 3);
            cairo_scale(cr, .55, .55);
            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
          }
          text(cr, properties->values[i].empty() ? "Default icon" : properties->values[i],
               125, y + 5, 260, 11);
          bevel(cr, 393, y + 2, 85, 25);
          text(cr, "Choose...", 401, y + 5, 76, 10);
          continue;
        }
        const bool selected = i == properties->field && properties->select_all &&
                              !properties->values[i].empty();
        if (selected) fill(cr, config.accent_color[0], config.accent_color[1],
                           config.accent_color[2], 98, y + 3, 379, 23);
        text(cr, properties->values[i] + (i == properties->field && !selected ? "|" : ""),
             99, y + 4, 375, 11, selected);
      }
      bevel(cr, 316, 184, 75, 27);
      bevel(cr, 400, 184, 75, 27);
      text(cr, "Save", 338, 188, 52, 11);
      text(cr, "Cancel", 416, 188, 55, 11);
    });
  }

  void filter_icon_choices() {
    if (!icon_chooser) return;
    auto& chooser = *icon_chooser;
    chooser.visible.clear();
    for (std::size_t i = 0; i < chooser.names.size(); ++i) {
      char* lower = g_ascii_strdown(chooser.names[i].c_str(), -1);
      const bool match = std::string_view(lower).find(chooser.search) != std::string_view::npos;
      g_free(lower);
      if (match) chooser.visible.push_back(i);
    }
    chooser.page = 0;
    chooser.selected.clear();
  }

  void draw_icon_chooser() {
    if (!icon_chooser) return;
    const auto& chooser = *icon_chooser;
    paint(chooser.window, icon_chooser_width, icon_chooser_height, [&](cairo_t* cr) {
      themed_fill(cr, config.surface_color, .75, .75, .75, 0, 0, icon_chooser_width, icon_chooser_height);
      bevel(cr, 0, 0, icon_chooser_width, icon_chooser_height);
      fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2],
           4, 4, icon_chooser_width - 8, 27);
      text(cr, "Choose an icon", 13, 8, 520, 12, true, true);
      text(cr, "Search", 12, 49, 58, 11);
      themed_fill(cr, config.field_color, 1, 1, 1, 72, 42, 496, 30);
      bevel(cr, 72, 42, 496, 30, true);
      text(cr, chooser.search + "|", 79, 48, 478, 11);
      const auto start = chooser.page * icon_chooser_page_size;
      const auto end = std::min(chooser.visible.size(), start + icon_chooser_page_size);
      if (chooser.visible.empty())
        text(cr, "No icons found", 225, 220, 200, 12);
      for (auto i = start; i < end; ++i) {
        const int cell = static_cast<int>(i - start);
        const int x = 10 + (cell % icon_chooser_columns) * icon_chooser_cell_width;
        const int y = 82 + (cell / icon_chooser_columns) * icon_chooser_cell_height;
        const auto& name = chooser.names[chooser.visible[i]];
        const bool selected = name == chooser.selected;
        if (selected) fill(cr, config.accent_color[0], config.accent_color[1],
                           config.accent_color[2], x, y, 108, 70);
        else themed_fill(cr, config.button_color, .88, .88, .88, x, y, 108, 70);
        bevel(cr, x, y, 108, 70, selected);
        if (auto* surface = object_icon_surface(name)) {
          cairo_set_source_surface(cr, surface, x + 34, y + 5);
          cairo_paint(cr);
        }
        text(cr, name, x + 5, y + 51, 99, 10, selected);
      }
      const auto pages = (chooser.visible.size() + icon_chooser_page_size - 1) /
                         icon_chooser_page_size;
      text(cr, "Page " + std::to_string(chooser.page + 1) + " / " +
               std::to_string(std::max<std::size_t>(1, pages)), 208, 393, 160, 10);
      for (const auto [x, width, label] : {
             std::tuple{10, 70, "Previous"}, std::tuple{88, 70, "Next"},
             std::tuple{262, 76, "Default"}, std::tuple{346, 102, "Choose"},
             std::tuple{456, 110, "Cancel"}}) {
        bevel(cr, x, 416, width, 27);
        text(cr, label, x + 7, 421, width - 12, 11);
      }
    });
  }

  void close_icon_chooser() {
    if (!icon_chooser) return;
    xcb_destroy_window(connection, icon_chooser->window);
    icon_chooser.reset();
    if (properties)
      xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT,
                          properties->window, XCB_CURRENT_TIME);
  }

  void show_icon_chooser() {
    if (!properties) return;
    if (icon_chooser) close_icon_chooser();
    IconChooser chooser;
    chooser.names = available_icon_names();
    chooser.selected = properties->values[1];
    chooser.visible.resize(chooser.names.size());
    std::iota(chooser.visible.begin(), chooser.visible.end(), 0);
    if (auto it = std::ranges::find(chooser.names, chooser.selected); it != chooser.names.end())
      chooser.page = static_cast<std::size_t>(it - chooser.names.begin()) / icon_chooser_page_size;
    const auto area = owner_area(properties->owner);
    const auto [x, y] = centered(area, icon_chooser_width, icon_chooser_height);
    chooser.window = create_window(x, y, icon_chooser_width, icon_chooser_height);
    icon_chooser = std::move(chooser);
    xcb_map_window(connection, icon_chooser->window);
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT,
                        icon_chooser->window, XCB_CURRENT_TIME);
    draw_icon_chooser();
  }

  void accept_icon_choice(bool use_default = false) {
    if (!properties || !icon_chooser || (!use_default && icon_chooser->selected.empty())) return;
    properties->values[1] = use_default ? std::string{} : icon_chooser->selected;
    close_icon_chooser();
    draw_properties();
  }

  void save_properties() {
    if (!properties) return;
    auto dialog = *properties;
    const auto parent = dialog.owner == desktop ? Store::desktop_path() :
      folders.contains(dialog.owner) ? folders.at(dialog.owner).path : dialog.item.path.parent_path();
    auto result = model.update_properties(dialog.item, parent, dialog.values[0], dialog.values[1], dialog.values[2]);
    if (!result) { show_notice("Properties failed: " + result.error()); return; }
    xcb_destroy_window(connection, properties->window);
    properties.reset();
    refresh();
  }

  void draw_notice() {
    if (!notice) return;
    paint(notice, 420, 100, [&](cairo_t* cr) {
      themed_fill(cr, config.surface_color, .75, .75, .75, 0, 0, 420, 100);
      bevel(cr, 0, 0, 420, 100);
      fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2],
           4, 4, 412, 25);
      text(cr, "Workspace", 12, 7, 390, 12, true, true);
      text(cr, notice_text, 14, 42, 390, 11);
      bevel(cr, 170, 70, 80, 22);
      text(cr, "OK", 199, 71, 50, 11);
    });
  }

  void draw_exit_dialog() {
    if (!exit_dialog) return;
    paint(exit_dialog, exit_dialog_width, exit_dialog_height, [&](cairo_t* cr) {
      themed_fill(cr, config.surface_color, .75, .75, .75, 0, 0, exit_dialog_width, exit_dialog_height);
      bevel(cr, 0, 0, exit_dialog_width, exit_dialog_height);
      fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2],
           4, 4, exit_dialog_width - 8, 25);
      text(cr, "Exit X11 session", 12, 7, 350, 12, true, true);
      themed_fill(cr, config.button_color, .87, .83, .93, 16, 42, 66, 66);
      bevel(cr, 16, 42, 66, 66, true);
      if (exit_logo_surface) {
        cairo_set_source_surface(cr, exit_logo_surface, 25, 51);
        cairo_paint(cr);
      } else {
        const auto color = config.dark_mode ? config.text_color : std::array<double, 3>{.25, .14, .38};
        cairo_set_source_rgb(cr, color[0], color[1], color[2]);
        cairo_set_line_width(cr, 4);
        cairo_arc(cr, 49, 75, 18, -.29 * std::numbers::pi, 1.29 * std::numbers::pi);
        cairo_stroke(cr);
        cairo_move_to(cr, 49, 54);
        cairo_line_to(cr, 49, 76);
        cairo_stroke(cr);
      }
      text(cr, "End this X11 session?", 96, 53, 265, 13, false, true);
      text(cr, "Open applications may close.", 96, 78, 265, 11);
      themed_fill(cr, config.shadow_color, .43, .43, .43, 12, 122, exit_dialog_width - 24, 1);
      themed_fill(cr, config.highlight_color, .93, .93, .93, 12, 123, exit_dialog_width - 24, 1);
      bevel(cr, 165, exit_dialog_button_y, 82, 26);
      text(cr, "Cancel", 183, exit_dialog_button_y + 4, 64, 11);
      bevel(cr, 265, exit_dialog_button_y, 90, 26);
      text(cr, "Exit", 294, exit_dialog_button_y + 4, 55, 11);
    });
  }

  void close_exit_dialog() {
    if (!exit_dialog) return;
    xcb_destroy_window(connection, exit_dialog);
    exit_dialog = 0;
  }

  void show_exit_dialog() {
    if (!exit_dialog) {
      const auto [x, y] = centered(primary_area(), exit_dialog_width, exit_dialog_height);
      exit_dialog = create_window(x, y, exit_dialog_width, exit_dialog_height);
      xcb_map_window(connection, exit_dialog);
    }
    const std::uint32_t above[]{XCB_STACK_MODE_ABOVE};
    xcb_configure_window(connection, exit_dialog, XCB_CONFIG_WINDOW_STACK_MODE, above);
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, exit_dialog, XCB_CURRENT_TIME);
    draw_exit_dialog();
    xcb_flush(connection);
  }

  void confirm_exit() {
    close_exit_dialog();
    if (transfers_in_progress > 0) {
      show_notice("Wait for file transfers to finish before exiting.");
      return;
    }
    if (exit_handler) exit_handler();
  }

  void close_submenu() {
    close_app_menu();
    if (submenu) xcb_destroy_window(connection, submenu);
    submenu = 0;
    submenu_entries.clear();
    submenu_hover = -1;
  }

  void close_app_menu() {
    if (app_menu) xcb_destroy_window(connection, app_menu);
    app_menu = 0;
    app_menu_entries.clear();
    app_menu_hover = -1;
  }

  void close_menu() {
    close_submenu();
    if (menu) xcb_destroy_window(connection, menu);
    menu = 0;
    menu_entries.clear();
    menu_hover = -1;
    xcb_flush(connection);
  }

  void show_menu(int x, int y, xcb_window_t owner, std::vector<MenuEntry> entries) {
    close_menu();
    menu_entries = std::move(entries);
    if (menu_entries.empty()) return;
    menu_owner = owner;
    const int h = static_cast<int>(menu_entries.size()) * row_height + 8;
    const auto area = area_at(x, y);
    menu_x = std::clamp(x, area.x, std::max(area.x, area.x + area.width - menu_width));
    menu_y = std::clamp(y, area.y, std::max(area.y, area.y + area.height - h));
    menu = create_window(menu_x, menu_y, menu_width, h);
    xcb_map_window(connection, menu);
    raise_panel();
    draw_menu();
  }

  void show_submenu(std::vector<MenuEntry> entries, int row) {
    close_submenu();
    submenu_entries = std::move(entries);
    if (submenu_entries.empty()) return;
    const int h = static_cast<int>(submenu_entries.size()) * row_height + 8;
    const int right = menu_x + menu_width - 1;
    const auto area = area_at(menu_x, menu_y);
    submenu_x = right + menu_width <= area.x + area.width ? right :
      std::max(area.x, menu_x - menu_width + 1);
    submenu_y = std::clamp(menu_y + 4 + row * row_height, area.y,
                            std::max(area.y, area.y + area.height - h));
    submenu = create_window(submenu_x, submenu_y, menu_width, h);
    xcb_map_window(connection, submenu);
    raise_panel();
    draw_submenu();
  }

  void show_app_menu(std::vector<MenuEntry> entries, int row) {
    close_app_menu();
    app_menu_entries = std::move(entries);
    if (app_menu_entries.empty()) return;
    const int h = static_cast<int>(app_menu_entries.size()) * row_height + 8;
    const int right = submenu_x + menu_width - 1;
    const auto area = area_at(submenu_x, submenu_y);
    const int x = right + menu_width <= area.x + area.width ? right :
      std::max(area.x, submenu_x - menu_width + 1);
    const int y = std::clamp(submenu_y + 4 + row * row_height, area.y,
                             std::max(area.y, area.y + area.height - h));
    app_menu = create_window(x, y, menu_width, h);
    xcb_map_window(connection, app_menu);
    raise_panel();
    draw_app_menu();
  }

  void show_launcher() {
    std::vector<MenuEntry> entries{{"Home", MenuAction::open_home}, {"Desktop", MenuAction::open_desktop},
                                   {"Programs ▶", MenuAction::show_programs},
                                   {"Recent Documents ▶", MenuAction::show_recent},
                                   {"New folder on desktop", MenuAction::new_folder},
                                   {"Paste on desktop", MenuAction::paste},
                                   {"Settings…", MenuAction::settings},
                                   {"Trash", MenuAction::open_trash},
                                   {"Exit X11 session…", MenuAction::exit_session}};
    const auto area = primary_area();
    show_menu(area.x + 3, area.y + panel_height, desktop, std::move(entries));
  }

  void lock_screen() {
    if (menu) close_menu();
    GError* error = nullptr;
    int status = 0;
    char command[] = "xscreensaver-command";
    char option[] = "-lock";
    char* argv[] = {command, option, nullptr};
    const bool launched = g_spawn_sync(nullptr, argv, nullptr,
                                       G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                                       nullptr, nullptr, &status, &error);
    if (!launched || !g_spawn_check_wait_status(status, &error)) {
      show_notice("Could not lock screen. Start xscreensaver first.");
    }
    if (error) g_error_free(error);
  }

  void show_categories(std::size_t page = 0) {
    std::vector<MenuEntry> entries;
    const auto page_size = static_cast<std::size_t>(std::clamp<int>((primary_area().height - 130) / row_height, 1, 18));
    category_page = std::min(page, app_categories.empty() ? 0 : (app_categories.size() - 1) / page_size);
    if (category_page) entries.push_back({"◀ Previous categories", MenuAction::previous_categories});
    const auto start = category_page * page_size;
    for (std::size_t i = start; i < std::min(app_categories.size(), start + page_size); ++i)
      entries.push_back({app_categories[i].label + " ▶", MenuAction::show_category, i});
    if (start + page_size < app_categories.size())
      entries.push_back({"More categories ▶", MenuAction::next_categories});
    show_submenu(std::move(entries), 2);
  }

  void show_programs() {
    reload_apps();
    if (app_categories.empty()) { show_notice("No applications available"); return; }
    show_categories();
  }

  void show_recent() {
    const auto limit = static_cast<std::size_t>(std::clamp<int>((primary_area().height - 130) / row_height, 1, 18));
    recent_menu_files = recent_files(limit);
    std::vector<MenuEntry> entries;
    for (std::size_t i = 0; i < recent_menu_files.size(); ++i)
      entries.push_back({recent_menu_files[i].title, MenuAction::open_recent, i});
    if (entries.empty()) entries.push_back({"No recent documents", MenuAction::noop});
    show_submenu(std::move(entries), 3);
  }

  void show_category(std::size_t category, std::size_t page = 0, int row = -1) {
    if (category >= app_categories.size()) return;
    selected_category = category;
    if (row >= 0) category_row = row;
    const auto& indices = app_categories[category].apps;
    const auto page_size = static_cast<std::size_t>(std::clamp<int>((primary_area().height - 130) / row_height, 1, 18));
    app_page = std::min(page, (indices.size() - 1) / page_size);
    std::vector<MenuEntry> entries;
    if (app_page) entries.push_back({"◀ Previous applications", MenuAction::previous_apps});
    const auto start = app_page * page_size;
    for (std::size_t i = start; i < std::min(indices.size(), start + page_size); ++i) {
      const auto app_index = indices[i];
      entries.push_back({g_app_info_get_display_name(apps[app_index]), MenuAction::launch, app_index});
    }
    if (start + page_size < indices.size())
      entries.push_back({"More applications ▶", MenuAction::next_apps});
    show_app_menu(std::move(entries), category_row);
  }

  void show_task_overflow(std::size_t page = 0) {
    const auto all_tasks = panel_tasks();
    const auto layout = task_layout(all_tasks.size());
    if (!layout.overflow) { close_menu(); return; }
    const std::size_t hidden = all_tasks.size() - layout.visible;
    const auto page_size = static_cast<std::size_t>(std::clamp<int>((primary_area().height - panel_height - 88) / row_height, 1, 18));
    task_page = std::min(page, (hidden - 1) / page_size);
    std::vector<MenuEntry> entries;
    if (task_page) entries.push_back({"◀ Previous windows", MenuAction::previous_tasks});
    const auto start = layout.visible + task_page * page_size;
    for (std::size_t i = start; i < std::min(all_tasks.size(), start + page_size); ++i)
      entries.push_back({(all_tasks[i].active ? "✓ " : "") + all_tasks[i].title,
                         MenuAction::activate_task, 0, all_tasks[i].window});
    if (start + page_size < all_tasks.size())
      entries.push_back({"More windows ▶", MenuAction::next_tasks});
    const auto area = primary_area();
    show_menu(area.x + layout.overflow_x, area.y + panel_height, panel, std::move(entries));
  }

  void show_context(xcb_window_t window, int root_x, int root_y, const Item* item) {
    if (item) selected_item = *item;
    else selected_item.reset();
    std::vector<MenuEntry> entries;
    if (item && item->trashed) {
      entries = {{"Restore", MenuAction::restore}, {"Delete permanently", MenuAction::delete_permanently},
                 {"Properties", MenuAction::properties}};
    } else if (item) {
      entries = {{"Open", MenuAction::open_item}};
      if (can_open_with(*item)) entries.push_back({"Open With…", MenuAction::open_with});
      entries.insert(entries.end(), {{"Copy", MenuAction::copy}, {"Cut", MenuAction::cut},
                 {"Create shadow", MenuAction::create_shadow}, {"Create template", MenuAction::create_template},
                 {"Properties", MenuAction::properties}, {"Move to Trash", MenuAction::trash}});
    } else if (!(folders.contains(window) && folders.at(window).trash)) {
      entries = {{"New folder", MenuAction::new_folder}, {"Paste", MenuAction::paste}};
    }
    show_menu(root_x, root_y, window, std::move(entries));
  }

  void show_folder_menu(Folder& folder, int tab) {
    selected_item = folder.selected >= 0 &&
      static_cast<std::size_t>(folder.selected) < folder.items.size()
      ? std::optional<Item>(folder.items[folder.selected]) : std::nullopt;
    std::vector<MenuEntry> entries;
    if (tab == 0) {
      if (selected_item) entries.push_back({"Open", MenuAction::open_item});
      if (selected_item && can_open_with(*selected_item))
        entries.push_back({"Open With…", MenuAction::open_with});
      if (!folder.trash) entries.push_back({"New folder", MenuAction::new_folder});
      if (selected_item) {
        entries.push_back({"Properties", MenuAction::properties});
        if (folder.trash) {
          entries.push_back({"Restore", MenuAction::restore});
          entries.push_back({"Delete permanently", MenuAction::delete_permanently});
        } else entries.push_back({"Move to Trash", MenuAction::trash});
      }
      entries.push_back({"Close", MenuAction::close_folder});
    } else if (tab == 1) {
      if (selected_item && !folder.trash) {
        entries.push_back({"Copy", MenuAction::copy});
        entries.push_back({"Cut", MenuAction::cut});
      }
      if (!folder.trash) entries.push_back({"Paste", MenuAction::paste});
    } else {
      entries = {{"Refresh", MenuAction::refresh_folder},
                 {folder.sort_by_type ? "Sort by Name" : "✓ Sort by Name", MenuAction::sort_by_name},
                 {folder.sort_by_type ? "✓ Sort by Type" : "Sort by Type", MenuAction::sort_by_type}};
    }
    show_menu(folder.x + (tab == 0 ? 8 : tab == 1 ? 52 : 98), folder.y + folder_header,
              folder.window, std::move(entries));
  }

  std::filesystem::path owner_path() const {
    if (const auto it = folders.find(menu_owner); it != folders.end()) return it->second.path;
    return Store::desktop_path();
  }

  bool open_folder(const std::filesystem::path& path,
                   const std::optional<std::filesystem::path>& selected = std::nullopt) {
    std::error_code ec;
    const bool trash = path == "trash:///";
    if (!trash && !std::filesystem::is_directory(path, ec)) {
      show_notice("Folder unavailable: " + path.string());
      return false;
    }
    Folder folder;
    folder.path = path;
    folder.trash = trash;
    const auto area = owner_area(menu_owner);
    const int offset = static_cast<int>(folders.size() % 10);
    folder.width = std::min(620, std::max(250, area.width - 30));
    folder.height = std::min(430, std::max(180, area.height - 60));
    folder.x = std::clamp(area.x + 85 + 24 * offset, area.x,
                          std::max(area.x, area.x + area.width - folder.width));
    const int top = area.y + (area.x == primary_area().x && area.y == primary_area().y ? panel_height : 0);
    folder.y = std::clamp(area.y + 75 + 22 * offset, top,
                          std::max(top, area.y + area.height - folder.height));
    folder.window = create_window(folder.x, folder.y, folder.width, folder.height,
                                  XCB_EVENT_MASK_FOCUS_CHANGE);
    folder.items = list_items(path, false);
    bool found = !selected;
    if (selected) {
      const auto normalized = selected->lexically_normal();
      for (std::size_t i = 0; i < folder.items.size(); ++i) {
        if (folder.items[i].path.lexically_normal() != normalized) continue;
        folder.selected = static_cast<int>(i);
        const auto scroll = folder_scroll(folder);
        const int columns = std::max(1, scroll.view_width / icon_width);
        folder.scroll_rows = std::max(0, static_cast<int>(i) / columns - scroll.visible_rows + 1);
        found = true;
        break;
      }
    }
    xcb_map_window(connection, folder.window);
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, folder.window, XCB_CURRENT_TIME);
    auto [it, inserted] = folders.emplace(folder.window, std::move(folder));
    if (inserted) { watch_directory(it->first, path); register_drop(it->first); draw_folder(it->second); draw_panel(); }
    return inserted && found;
  }

  void open_item(const Item& item) {
    if (item.object.kind == ObjectKind::trash) {
      open_folder("trash:///");
      return;
    }
    if (item.trashed) { show_notice("Restore this item before opening it"); return; }
    if (item.path.empty()) { show_notice("This shadow's target no longer exists"); return; }
    auto path = item.path;
    if (item.object.kind == ObjectKind::template_object) {
      path = unique_path(owner_path() / basename(path));
      if (auto result = copy_tree(item.path, path); !result) {
        show_notice("Template failed: " + result.error());
        return;
      }
      refresh();
    }
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) { open_folder(path); return; }
    if (path.extension() == ".desktop") {
      if (auto launched = launch_desktop_entry(path, Store::desktop_path(), startup); !launched)
        show_notice("Launcher: " + launched.error());
      return;
    }
    const auto uri = file_uri(path);
    GError* error = nullptr;
    const bool launched = startup ? startup->launch_default_uri(uri.c_str(), &error) :
      g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error);
    if (launched) remember_file(path);
    if (error) {
      show_notice(std::string("Open failed: ") + error->message);
      g_error_free(error);
    }
  }

  void execute(MenuEntry entry, int row = -1) {
    const auto destination = owner_path();
    if (entry.action != MenuAction::show_programs && entry.action != MenuAction::show_recent &&
        entry.action != MenuAction::next_apps &&
        entry.action != MenuAction::previous_apps && entry.action != MenuAction::show_category &&
        entry.action != MenuAction::next_categories &&
        entry.action != MenuAction::previous_categories) close_menu();
    switch (entry.action) {
      case MenuAction::noop: break;
      case MenuAction::settings: {
        GError* error = nullptr;
        auto* process = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
                                        settings_program.c_str(), nullptr);
        if (process) g_object_unref(process);
        else show_notice("Could not open MidnightBSD Settings.");
        if (error) g_error_free(error);
        break;
      }
      case MenuAction::open_home: open_folder(g_get_home_dir()); break;
      case MenuAction::open_desktop: open_folder(Store::desktop_path()); break;
      case MenuAction::open_trash: open_folder("trash:///"); break;
      case MenuAction::show_recent: show_recent(); break;
      case MenuAction::open_recent: {
        if (entry.index >= recent_menu_files.size()) break;
        const auto path = recent_menu_files[entry.index].path;
        const auto uri = file_uri(path);
        GError* error = nullptr;
        const bool launched = startup ? startup->launch_default_uri(uri.c_str(), &error) :
          g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error);
        if (launched) remember_file(path);
        if (error) { show_notice(std::string("Open failed: ") + error->message); g_error_free(error); }
        break;
      }
      case MenuAction::new_folder: {
        auto result = model.create_folder(destination);
        if (result) refresh(); else show_notice("Could not create folder: " + result.error());
        break;
      }
      case MenuAction::paste: {
        auto paste = [this, destination](const std::vector<std::filesystem::path>& paths, bool cut) {
          const bool own_cut = cut && !clipboard_path.empty() && std::ranges::find(paths, clipboard_path) != paths.end();
          transfer_async(paths, destination, cut, [this, own_cut](std::expected<void, std::string> result) {
            --transfers_in_progress;
            if (!result) show_notice("Paste failed: " + result.error());
            else {
              if (own_cut) { clipboard_path.clear(); if (clipboard_set) clipboard_set({}, false); }
              refresh();
            }
            draw_panel();
          });
        };
        if (clipboard_request) clipboard_request(std::move(paste));
        else if (!clipboard_path.empty()) paste({clipboard_path}, clipboard_cut);
        break;
      }
      case MenuAction::launch: {
        if (entry.index >= apps.size()) break;
        GError* error = nullptr;
        if (startup) startup->launch(apps[entry.index], nullptr, &error);
        else g_app_info_launch(apps[entry.index], nullptr, nullptr, &error);
        if (error) {
          show_notice(std::string("Launch failed: ") + error->message);
          g_error_free(error);
        }
        break;
      }
      case MenuAction::show_programs: show_programs(); break;
      case MenuAction::show_category: show_category(entry.index, 0, row); break;
      case MenuAction::next_categories: show_categories(category_page + 1); break;
      case MenuAction::previous_categories: show_categories(category_page ? category_page - 1 : 0); break;
      case MenuAction::next_apps: show_category(selected_category, app_page + 1); break;
      case MenuAction::previous_apps: show_category(selected_category, app_page ? app_page - 1 : 0); break;
      case MenuAction::activate_task: activate_task_window(entry.window, XCB_CURRENT_TIME); break;
      case MenuAction::next_tasks: show_task_overflow(task_page + 1); break;
      case MenuAction::previous_tasks: show_task_overflow(task_page ? task_page - 1 : 0); break;
      case MenuAction::open_item: if (selected_item) open_item(*selected_item); break;
      case MenuAction::open_with: if (selected_item) show_open_with_dialog(*selected_item); break;
      case MenuAction::copy:
      case MenuAction::cut:
        if (selected_item) {
          clipboard_path = selected_item->path;
          clipboard_cut = entry.action == MenuAction::cut;
          if (clipboard_set) clipboard_set({clipboard_path}, clipboard_cut);
        }
        break;
      case MenuAction::create_shadow:
      case MenuAction::create_template:
        if (selected_item) {
          auto result = model.add_reference(*selected_item, destination,
                            entry.action == MenuAction::create_shadow ? ObjectKind::shadow : ObjectKind::template_object);
          if (result) refresh(); else show_notice("Reference failed: " + result.error());
        }
        break;
      case MenuAction::properties:
        if (selected_item) show_properties(*selected_item);
        break;
      case MenuAction::trash:
        if (selected_item) {
          auto result = model.remove(*selected_item);
          if (result) refresh(); else show_notice("Trash failed: " + result.error());
        }
        break;
      case MenuAction::restore:
        if (selected_item && selected_item->trashed) {
          auto result = model.restore(*selected_item);
          if (result) refresh(); else show_notice("Restore failed: " + result.error());
        }
        break;
      case MenuAction::delete_permanently:
        if (selected_item && selected_item->trashed) {
          auto result = model.delete_permanently(*selected_item);
          if (result) refresh(); else show_notice("Delete failed: " + result.error());
        }
        break;
      case MenuAction::close_folder:
        if (folders.contains(menu_owner)) {
          unwatch(menu_owner);
          if (dnd_unregister) dnd_unregister(menu_owner);
          xcb_destroy_window(connection, menu_owner);
          folders.erase(menu_owner);
          draw_panel();
        }
        break;
      case MenuAction::exit_session: show_exit_dialog(); break;
      case MenuAction::refresh_folder: refresh(); break;
      case MenuAction::sort_by_name:
      case MenuAction::sort_by_type:
        if (auto it = folders.find(menu_owner); it != folders.end()) {
          it->second.sort_by_type = entry.action == MenuAction::sort_by_type;
          it->second.items = list_items(it->second.path, false);
          sort_folder(it->second);
          it->second.selected = -1;
          draw_folder(it->second);
        }
        break;
    }
  }

  int item_index(int x, int y, int start_y, int width_px, std::size_t count, int scroll_rows = 0) const {
    if (x < 12 || y < start_y) return -1;
    const int cols = std::max(1, width_px / icon_width);
    const int col = (x - 12) / icon_width;
    const int row = (y - start_y) / icon_height + scroll_rows;
    if (col >= cols || (x - 12) % icon_width >= 78 || (y - start_y) % icon_height >= 78) return -1;
    const int index = row * cols + col;
    return index >= 0 && static_cast<std::size_t>(index) < count ? index : -1;
  }

  void button(const xcb_button_press_event_t* event) {
    if (open_with_dialog && event->event == open_with_dialog->window) {
      auto& dialog = *open_with_dialog;
      if (event->detail == 4 || event->detail == 5) {
        const auto max_scroll = dialog.apps.size() > open_with_rows ?
          dialog.apps.size() - open_with_rows : 0;
        if (event->detail == 4 && dialog.scroll) --dialog.scroll;
        else if (event->detail == 5 && dialog.scroll < max_scroll) ++dialog.scroll;
        draw_open_with_dialog();
      } else if (event->detail == 1) {
        if (event->event_y >= 57 && event->event_y < 337 &&
            event->event_x >= 12 && event->event_x < open_with_width - 12) {
          const auto index = dialog.scroll + static_cast<std::size_t>((event->event_y - 57) / 28);
          if (index < dialog.apps.size()) { dialog.selected = index; draw_open_with_dialog(); }
        } else if (event->event_y >= 355 && event->event_y < 381) {
          if (event->event_x >= 10 && event->event_x < 110) accept_open_with(false);
          else if (event->event_x >= 118 && event->event_x < 308) accept_open_with(true);
          else if (event->event_x >= 316 && event->event_x < 448) close_open_with_dialog();
        }
      }
      return;
    }
    if (open_with_dialog) return;
    if (icon_chooser && event->event == icon_chooser->window) {
      auto& chooser = *icon_chooser;
      const auto pages = (chooser.visible.size() + icon_chooser_page_size - 1) /
                         icon_chooser_page_size;
      if (event->detail == 4 || event->detail == 5) {
        if (event->detail == 4 && chooser.page) --chooser.page;
        else if (event->detail == 5 && chooser.page + 1 < pages) ++chooser.page;
        draw_icon_chooser();
      } else if (event->detail == 1) {
        const int x = event->event_x - 10;
        const int y = event->event_y - 82;
        if (x >= 0 && x < icon_chooser_columns * icon_chooser_cell_width && y >= 0 &&
            y < icon_chooser_rows * icon_chooser_cell_height &&
            x % icon_chooser_cell_width < 108 && y % icon_chooser_cell_height < 70) {
          const auto index = chooser.page * icon_chooser_page_size +
            static_cast<std::size_t>(y / icon_chooser_cell_height * icon_chooser_columns +
                                     x / icon_chooser_cell_width);
          if (index < chooser.visible.size()) {
            chooser.selected = chooser.names[chooser.visible[index]];
            draw_icon_chooser();
          }
        } else if (event->event_y >= 416 && event->event_y < 443) {
          if (event->event_x >= 10 && event->event_x < 80 && chooser.page) {
            --chooser.page;
            draw_icon_chooser();
          } else if (event->event_x >= 88 && event->event_x < 158 && chooser.page + 1 < pages) {
            ++chooser.page;
            draw_icon_chooser();
          } else if (event->event_x >= 262 && event->event_x < 338) accept_icon_choice(true);
          else if (event->event_x >= 346 && event->event_x < 448) accept_icon_choice();
          else if (event->event_x >= 456 && event->event_x < 566) close_icon_chooser();
        }
      }
      return;
    }
    if (icon_chooser) return;
    if (exit_dialog && event->event == exit_dialog) {
      if (event->detail == 1 && event->event_y >= exit_dialog_button_y &&
          event->event_y < exit_dialog_button_y + 26) {
        if (event->event_x >= 265 && event->event_x < 355) confirm_exit();
        else if (event->event_x >= 165 && event->event_x < 247) close_exit_dialog();
      }
      return;
    }
    if (properties && event->event == properties->window) {
      if (event->event_y >= 184 && event->event_y <= 211) {
        if (event->event_x >= 316 && event->event_x <= 391) save_properties();
        else if (event->event_x >= 400 && event->event_x <= 475) {
          xcb_destroy_window(connection, properties->window);
          properties.reset();
        }
      } else if (event->detail == 1 && event->event_x >= 92 && event->event_x < 482 &&
                 event->event_y >= 47 && event->event_y < 166) {
        const int field = (event->event_y - 47) / 45;
        const int field_y = 47 + field * 45;
        if (field == 1 && event->event_y < field_y + 29) {
          show_icon_chooser();
        } else if (event->event_y < field_y + 29 &&
                   !(properties->item.physical && field == 2)) {
          properties->select_all = properties->last_click_field == field &&
            event->time - properties->last_click_time < 400;
          properties->last_click_field = properties->select_all ? -1 : field;
          properties->last_click_time = event->time;
          properties->field = field;
          draw_properties();
        }
      }
      return;
    }
    if (event->event == notice) {
      xcb_destroy_window(connection, notice);
      notice = 0;
      return;
    }
    if (event->event == menu || event->event == submenu || event->event == app_menu) {
      const auto& entries = event->event == menu ? menu_entries :
                            event->event == submenu ? submenu_entries : app_menu_entries;
      const int index = (event->event_y - 4) / row_height;
      if (event->detail == 1 && event->event_y >= 4 && index >= 0 && static_cast<std::size_t>(index) < entries.size())
        execute(entries[index], index);
      else close_menu();
      return;
    }
    if (event->event == panel) {
      const int volume_x = this->volume_x();
      if (show_volume() && event->event_x >= volume_x &&
          event->event_x < volume_x + volume_group_width) {
        std::expected<VolumeState, std::string> changed = std::unexpected("No volume action");
        const int relative_x = event->event_x - volume_x;
        if (event->detail == 4 || (event->detail == 1 && relative_x >= 64 && relative_x < 82))
          changed = volume.adjust(5);
        else if (event->detail == 5 || (event->detail == 1 && relative_x < 18)) changed = volume.adjust(-5);
        else if (event->detail == 3 || (event->detail == 1 && relative_x >= 84)) changed = volume.toggle_mute();
        else return;
        if (changed) volume_state = *changed;
        else { volume_state.reset(); show_notice("Volume: " + changed.error()); }
        draw_panel();
        return;
      }
      if (event->detail == 1 && event->event_x < 122) { if (menu) close_menu(); else show_launcher(); }
      else if (event->detail == 1 && event->event_x >= lock_button_x &&
               event->event_x < lock_button_x + lock_button_width) lock_screen();
      else if (event->detail == 1 && event->event_x >= terminal_button_x &&
               event->event_x < terminal_button_x + terminal_button_width) {
        auto result = launch_terminal(startup);
        if (!result) show_notice("Terminal: " + result.error());
      }
      else if (event->detail == 1 && event->event_x >= browser_button_x &&
               event->event_x < browser_button_x + browser_button_width) {
        auto result = launch_web_browser(startup);
        if (!result) show_notice("Browser: " + result.error());
      }
      else if (event->detail == 1 && event->event_x >= task_start_x) {
        const auto all_tasks = panel_tasks();
        const auto layout = task_layout(all_tasks.size());
        if (layout.overflow && event->event_x >= layout.overflow_x &&
            event->event_x < layout.overflow_x + task_overflow_width) {
          if (menu && menu_owner == panel) close_menu();
          else show_task_overflow();
          return;
        }
        if (event->event_x < task_start_x + static_cast<int>(layout.visible) * task_step) {
          const int index = (event->event_x - task_start_x) / task_step;
          if ((event->event_x - task_start_x) % task_step < task_button_width) {
            if (menu) close_menu();
            activate_task_window(all_tasks[index].window, event->time);
          }
        }
      }
      return;
    }
    if (menu) close_menu();
    auto* folder = [&]() -> Folder* { auto it = folders.find(event->event); return it == folders.end() ? nullptr : &it->second; }();
    if (folder && (event->detail == 4 || event->detail == 5)) {
      const auto scroll = folder_scroll(*folder);
      folder->scroll_rows = std::clamp(folder->scroll_rows + (event->detail == 5 ? 1 : -1), 0,
                                       scroll.max_scroll);
      draw_folder(*folder);
      return;
    }
    if (folder && event->event_y < 31) {
      if (event->event_x > folder->width - 34) {
        const auto window = folder->window;
        unwatch(window);
        if (dnd_unregister) dnd_unregister(window);
        xcb_destroy_window(connection, window);
        folders.erase(window);
        if (focused_folder == window) focused_folder = 0;
        draw_panel();
      }
      else {
        xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, folder->window, event->time);
        if (event->detail == 1) {
          drag_window = folder->window;
          resizing = false;
          drag_root_x = event->root_x;
          drag_root_y = event->root_y;
          drag_start_x = folder->x;
          drag_start_y = folder->y;
          xcb_grab_pointer(connection, 0, folder->window, XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                           XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, event->time);
        }
      }
      return;
    }
    if (folder && event->detail == 1 && event->event_x >= folder->width - 17 && event->event_y >= folder->height - 17) {
      drag_window = folder->window;
      resizing = true;
      drag_root_x = event->root_x;
      drag_root_y = event->root_y;
      drag_start_x = folder->width;
      drag_start_y = folder->height;
      xcb_grab_pointer(connection, 0, folder->window, XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                       XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, event->time);
      return;
    }
    if (folder && event->detail == 1 && event->event_y >= 31 && event->event_y < folder_header) {
      const int tab = event->event_x >= 8 && event->event_x < 50 ? 0 :
                      event->event_x >= 52 && event->event_x < 96 ? 1 :
                      event->event_x >= 98 && event->event_x < 150 ? 2 : -1;
      if (tab >= 0) show_folder_menu(*folder, tab);
      return;
    }
    if (folder) {
      const auto scroll = folder_scroll(*folder);
      if (event->event_y >= folder_header && event->event_y < folder_header + scroll.view_height &&
          event->event_x >= scroll.x) {
        if (event->detail != 1) return;
        pending_icon_window = 0;
        xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, folder->window, event->time);
        if (event->event_y < scroll.track_y) --folder->scroll_rows;
        else if (event->event_y >= scroll.track_y + scroll.track_height) ++folder->scroll_rows;
        else if (event->event_y < scroll.thumb_y) folder->scroll_rows -= scroll.visible_rows;
        else if (event->event_y >= scroll.thumb_y + scroll.thumb_height)
          folder->scroll_rows += scroll.visible_rows;
        else if (scroll.max_scroll) {
          scroll_drag_window = folder->window;
          scroll_grab_offset = event->event_y - scroll.thumb_y;
          xcb_grab_pointer(connection, 0, folder->window,
                           XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                           XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, event->time);
        }
        folder->scroll_rows = std::clamp(folder->scroll_rows, 0, scroll.max_scroll);
        draw_folder(*folder);
        return;
      }
      if (event->event_y < folder_header || event->event_y >= folder->height - 22) return;
    }
    const bool on_desktop = event->event == desktop;
    if (!on_desktop && !folder) return;
    if (on_desktop && (event->detail == 4 || event->detail == 5)) {
      desktop_scroll_rows = std::clamp(desktop_scroll_rows + (event->detail == 5 ? 1 : -1),
                                       0, desktop_max_scroll());
      draw_desktop();
      return;
    }
    auto& items = on_desktop ? desktop_items : folder->items;
    const int start_y = on_desktop ? 13 : folder_header + 11;
    const auto area = primary_area();
    const int icon_x = on_desktop ? event->event_x - area.x : event->event_x;
    const int icon_y = on_desktop ? event->event_y - area.y - panel_height : event->event_y;
    const int width_px = on_desktop ? desktop_width : folder_scroll(*folder).view_width;
    const int index = on_desktop && (icon_x < 0 || icon_x >= desktop_width ||
                                   icon_y < 0 || icon_y >= desktop_height) ? -1 :
      item_index(icon_x, icon_y, start_y, width_px, items.size(),
                 folder ? folder->scroll_rows : desktop_scroll_rows);
    if (event->detail == 3) {
      show_context(event->event, event->root_x, event->root_y, index >= 0 ? &items[index] : nullptr);
      return;
    }
    if (event->detail != 1) return;
    if (on_desktop) {
      selected_desktop = index;
      draw_desktop();
      xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, desktop, event->time);
    }
    else { folder->selected = index; draw_folder(*folder); xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, folder->window, event->time); }
    pending_icon_window = event->event;
    pending_icon_index = index;
    pending_icon_x = event->root_x;
    pending_icon_y = event->root_y;
    if (index >= 0 && last_click_window == event->event && last_click_index == index &&
        event->time - last_click_time < 400) {
      menu_owner = event->event;
      open_item(items[index]);
      last_click_time = 0;
    } else { last_click_time = event->time; last_click_index = index; last_click_window = event->event; }
  }

  void key(const xcb_key_press_event_t* event) {
    if (open_with_dialog && event->event == open_with_dialog->window) {
      auto& dialog = *open_with_dialog;
      const auto sym = key_symbols ? xcb_key_symbols_get_keysym(key_symbols, event->detail, 0) : 0;
      if (sym == XK_Escape) { close_open_with_dialog(); return; }
      if (sym == XK_Return) { accept_open_with(false); return; }
      if (sym == XK_Up && dialog.selected) --dialog.selected;
      else if (sym == XK_Down && dialog.selected + 1 < dialog.apps.size()) ++dialog.selected;
      else if (sym == XK_Page_Up)
        dialog.selected = dialog.selected > open_with_rows ? dialog.selected - open_with_rows : 0;
      else if (sym == XK_Page_Down)
        dialog.selected = std::min(dialog.apps.size() - 1, dialog.selected + open_with_rows);
      else return;
      if (dialog.selected < dialog.scroll) dialog.scroll = dialog.selected;
      else if (dialog.selected >= dialog.scroll + open_with_rows)
        dialog.scroll = dialog.selected - open_with_rows + 1;
      draw_open_with_dialog();
      return;
    }
    if (open_with_dialog) return;
    if (icon_chooser && event->event == icon_chooser->window) {
      auto& chooser = *icon_chooser;
      const auto sym = key_symbols ? xcb_key_symbols_get_keysym(key_symbols, event->detail,
                                                       (event->state & XCB_MOD_MASK_SHIFT) ? 1 : 0) : 0;
      const auto pages = (chooser.visible.size() + icon_chooser_page_size - 1) /
                         icon_chooser_page_size;
      if (sym == XK_Escape) { close_icon_chooser(); return; }
      if (sym == XK_Return) { accept_icon_choice(); return; }
      if (sym == XK_Page_Up || sym == XK_Left) {
        if (chooser.page) --chooser.page;
      } else if (sym == XK_Page_Down || sym == XK_Right) {
        if (chooser.page + 1 < pages) ++chooser.page;
      } else if (sym == XK_BackSpace) {
        if (!chooser.search.empty()) {
          chooser.search.pop_back();
          filter_icon_choices();
        }
      } else if (!(event->state & XCB_MOD_MASK_CONTROL) && sym >= 0x20 && sym <= 0x7e) {
        chooser.search += static_cast<char>(g_ascii_tolower(sym));
        filter_icon_choices();
      }
      draw_icon_chooser();
      return;
    }
    if (icon_chooser) return;
    if (exit_dialog && event->event == exit_dialog) {
      const auto sym = key_symbols ? xcb_key_symbols_get_keysym(key_symbols, event->detail, 0) : 0;
      if (sym == XK_Escape) close_exit_dialog();
      else if (sym == XK_Return) confirm_exit();
      return;
    }
    if (properties && event->event == properties->window) {
      const auto sym = key_symbols ? xcb_key_symbols_get_keysym(key_symbols, event->detail,
                                                       (event->state & XCB_MOD_MASK_SHIFT) ? 1 : 0) : 0;
      if (sym == XK_Escape) { xcb_destroy_window(connection, properties->window); properties.reset(); return; }
      if (sym == XK_Return) { save_properties(); return; }
      if (sym == XK_Tab) {
        properties->field = properties->field == 0 && !properties->item.physical ? 2 : 0;
        properties->select_all = false;
      } else if ((event->state & XCB_MOD_MASK_CONTROL) && (sym == XK_a || sym == XK_A)) {
        properties->select_all = true;
      } else if (sym == XK_BackSpace || sym == XK_Delete) {
        auto& value = properties->values[properties->field];
        if (properties->select_all) value.clear();
        else if (!value.empty()) value.pop_back();
        properties->select_all = false;
      } else if (!(event->state & XCB_MOD_MASK_CONTROL) && sym >= 0x20 && sym <= 0x7e) {
        auto& value = properties->values[properties->field];
        if (properties->select_all) value.clear();
        value += static_cast<char>(sym);
        properties->select_all = false;
      }
      draw_properties();
      return;
    }
    auto it = folders.find(event->event);
    const bool on_desktop = event->event == desktop;
    if (!on_desktop && it == folders.end()) return;
    auto& items = on_desktop ? desktop_items : it->second.items;
    int& selected = on_desktop ? selected_desktop : it->second.selected;
    const auto sym = key_symbols ? xcb_key_symbols_get_keysym(key_symbols, event->detail, 0) : 0;
    if (sym == XK_Escape) { close_menu(); return; }
    if (on_desktop && (sym == XK_Page_Up || sym == XK_Page_Down || sym == XK_Home || sym == XK_End)) {
      if (sym == XK_Home) desktop_scroll_rows = 0;
      else if (sym == XK_End) desktop_scroll_rows = desktop_max_scroll();
      else desktop_scroll_rows = std::clamp(desktop_scroll_rows +
        (sym == XK_Page_Down ? desktop_visible_rows() : -desktop_visible_rows()), 0,
        desktop_max_scroll());
      draw_desktop();
      return;
    }
    if (!on_desktop && (sym == XK_Page_Up || sym == XK_Page_Down || sym == XK_Home || sym == XK_End)) {
      const auto scroll = folder_scroll(it->second);
      if (sym == XK_Home) it->second.scroll_rows = 0;
      else if (sym == XK_End) it->second.scroll_rows = scroll.max_scroll;
      else it->second.scroll_rows = std::clamp(it->second.scroll_rows +
        (sym == XK_Page_Down ? scroll.visible_rows : -scroll.visible_rows), 0, scroll.max_scroll);
      draw_folder(it->second);
      return;
    }
    if (sym == XK_Return && selected >= 0 && static_cast<std::size_t>(selected) < items.size()) {
      menu_owner = event->event;
      open_item(items[selected]);
    }
    if ((event->state & XCB_MOD_MASK_CONTROL) && selected >= 0 && static_cast<std::size_t>(selected) < items.size()) {
      if (sym == XK_c || sym == XK_x) {
        clipboard_path = items[selected].path;
        clipboard_cut = sym == XK_x;
        if (clipboard_set) clipboard_set({clipboard_path}, clipboard_cut);
      }
    }
    if ((event->state & XCB_MOD_MASK_CONTROL) && sym == XK_v) {
      menu_owner = event->event;
      execute({"Paste", MenuAction::paste});
    }
    if ((event->state & XCB_MOD_MASK_CONTROL) && sym == XK_n) {
      menu_owner = event->event;
      execute({"New folder", MenuAction::new_folder});
    }
    if (sym == XK_Delete && selected >= 0 && static_cast<std::size_t>(selected) < items.size()) {
      selected_item = items[selected];
      menu_owner = event->event;
      execute({"Trash", MenuAction::trash});
    }
  }

  void motion(const xcb_motion_notify_event_t* event) {
    if (event->event == menu || event->event == submenu || event->event == app_menu) {
      const auto& entries = event->event == menu ? menu_entries :
                            event->event == submenu ? submenu_entries : app_menu_entries;
      int& hover = event->event == menu ? menu_hover :
                   event->event == submenu ? submenu_hover : app_menu_hover;
      const int index = (event->event_y - 4) / row_height;
      const int next = event->event_y >= 4 && index >= 0 &&
        static_cast<std::size_t>(index) < entries.size() ? index : -1;
      if (next != hover) {
        hover = next;
        if (event->event == menu) draw_menu();
        else if (event->event == submenu) {
          draw_submenu();
          if (next >= 0 && entries[next].action == MenuAction::show_category)
            show_category(entries[next].index, 0, next);
          else close_app_menu();
        } else draw_app_menu();
      }
      return;
    }
    if (scroll_drag_window) {
      auto it = folders.find(scroll_drag_window);
      if (it == folders.end()) return;
      auto& folder = it->second;
      const auto scroll = folder_scroll(folder);
      const int travel = scroll.track_height - scroll.thumb_height;
      if (travel > 0 && scroll.max_scroll > 0) {
        const int position = std::clamp(event->event_y - scroll_grab_offset - scroll.track_y, 0, travel);
        const int row = (position * scroll.max_scroll + travel / 2) / travel;
        if (row != folder.scroll_rows) { folder.scroll_rows = row; draw_folder(folder); }
      }
      return;
    }
    if (pending_icon_window && dnd_begin) {
      const int dx = event->root_x - pending_icon_x;
      const int dy = event->root_y - pending_icon_y;
      if (dx * dx + dy * dy >= 49) {
        auto* items = pending_icon_window == desktop ? &desktop_items :
          folders.contains(pending_icon_window) ? &folders.at(pending_icon_window).items : nullptr;
        if (items && pending_icon_index >= 0 && static_cast<std::size_t>(pending_icon_index) < items->size()) {
          const auto path = (*items)[pending_icon_index].path;
          if (!path.empty()) dnd_begin(pending_icon_window, {path}, true);
        }
        pending_icon_window = 0;
      }
      return;
    }
    if (!drag_window) return;
    auto it = folders.find(drag_window);
    if (it == folders.end()) return;
    auto& folder = it->second;
    const int dx = event->root_x - drag_root_x;
    const int dy = event->root_y - drag_root_y;
    if (resizing) {
      folder.width = std::max(250, drag_start_x + dx);
      folder.height = std::max(180, drag_start_y + dy);
      const std::uint32_t values[]{static_cast<std::uint32_t>(scaled_pixel(scale, folder.width)),
                                   static_cast<std::uint32_t>(scaled_pixel(scale, folder.height))};
      xcb_configure_window(connection, folder.window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
      draw_folder(folder);
    } else {
      folder.x = drag_start_x + dx;
      folder.y = drag_start_y + dy;
      const std::uint32_t values[]{static_cast<std::uint32_t>(std::lround(folder.x * scale)),
                                   static_cast<std::uint32_t>(std::lround(folder.y * scale))};
      xcb_configure_window(connection, folder.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
    }
    xcb_flush(connection);
  }
};

Shell::Shell(xcb_connection_t* connection, xcb_screen_t* screen, Store& store)
  : impl_(std::make_unique<Impl>(connection, screen, store)) {}
Shell::~Shell() = default;

bool Shell::initialize() {
  auto& p = *impl_;
  p.visual = visual_for(p.screen);
  if (!p.visual) return false;
  p.load_logo();
  if (auto state = p.volume.read(); state) p.volume_state = *state;
  p.last_volume_poll = std::chrono::steady_clock::now();
  p.key_symbols = xcb_key_symbols_alloc(p.connection);
  auto atom_cookie = xcb_intern_atom(p.connection, 0, 18, "_NET_ACTIVE_WINDOW");
  if (auto* reply = xcb_intern_atom_reply(p.connection, atom_cookie, nullptr)) {
    p.active_atom = reply->atom;
    std::free(reply);
  }
  std::error_code ec;
  std::filesystem::create_directories(Store::desktop_path(), ec);
  p.seed_objects();
  p.update_monitor_geometry();
  const auto primary = p.primary_area();
  p.desktop = p.create_window(0, 0, p.width, p.height);
  p.panel = p.create_window(primary.x, primary.y, p.panel_width, Impl::panel_height);
  const auto panel_pixels = scaled_pixel(p.scale, Impl::panel_height);
  const std::uint32_t desktop_size[]{p.physical_width, p.physical_height};
  xcb_configure_window(p.connection, p.desktop,
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, desktop_size);
  const std::uint32_t panel_size[]{static_cast<std::uint32_t>(p.primary_monitor.width),
                                    static_cast<std::uint32_t>(panel_pixels)};
  xcb_configure_window(p.connection, p.panel,
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, panel_size);
  p.watch_directory(p.desktop, Store::desktop_path());
  p.register_drop(p.desktop);
  xcb_map_window(p.connection, p.desktop);
  xcb_map_window(p.connection, p.panel);
  const std::uint32_t below[]{XCB_STACK_MODE_BELOW};
  xcb_configure_window(p.connection, p.desktop, XCB_CONFIG_WINDOW_STACK_MODE, below);
  p.raise_panel();
  p.last_clock = clock_text(p.config.clock_format);
  p.refresh();
  p.draw_panel();
  xcb_flush(p.connection);
  return true;
}

bool Shell::owns(xcb_window_t window) const {
  const auto& p = *impl_;
  return window && (window == p.desktop || window == p.panel || window == p.menu ||
                    window == p.submenu || window == p.app_menu || window == p.notice ||
                    window == p.exit_dialog ||
                    (p.icon_chooser && window == p.icon_chooser->window) ||
                    (p.open_with_dialog && window == p.open_with_dialog->window) ||
                    (p.properties && window == p.properties->window) || p.folders.contains(window));
}

bool Shell::handle(xcb_generic_event_t* event) {
  if (!event) return false;
  auto& p = *impl_;
  const auto type = event->response_type & ~0x80;
  xcb_window_t window{};
  switch (type) {
    case XCB_EXPOSE: window = reinterpret_cast<xcb_expose_event_t*>(event)->window; break;
    case XCB_BUTTON_PRESS: window = reinterpret_cast<xcb_button_press_event_t*>(event)->event; break;
    case XCB_BUTTON_RELEASE: window = reinterpret_cast<xcb_button_release_event_t*>(event)->event; break;
    case XCB_MOTION_NOTIFY: window = reinterpret_cast<xcb_motion_notify_event_t*>(event)->event; break;
    case XCB_KEY_PRESS: window = reinterpret_cast<xcb_key_press_event_t*>(event)->event; break;
    case XCB_FOCUS_IN:
    case XCB_FOCUS_OUT: window = reinterpret_cast<xcb_focus_in_event_t*>(event)->event; break;
    case XCB_DESTROY_NOTIFY: window = reinterpret_cast<xcb_destroy_notify_event_t*>(event)->window; break;
    default: return false;
  }
  if (!owns(window)) return false;
  if (p.startup && type == XCB_BUTTON_PRESS)
    p.startup->record_user_time(reinterpret_cast<xcb_button_press_event_t*>(event)->time);
  else if (p.startup && type == XCB_KEY_PRESS)
    p.startup->record_user_time(reinterpret_cast<xcb_key_press_event_t*>(event)->time);
  if (type == XCB_FOCUS_IN && p.folders.contains(window)) {
    if (p.focused_folder != window) { p.focused_folder = window; p.draw_panel(); }
  } else if (type == XCB_FOCUS_OUT && p.focused_folder == window) {
    p.focused_folder = 0;
    p.draw_panel();
  } else if (type == XCB_EXPOSE) {
    if (window == p.panel) p.draw_panel();
    else if (window == p.desktop) {
      if (!p.desktop_pixmap) p.draw_desktop();
    }
    else if (window == p.menu) p.draw_menu();
    else if (window == p.submenu) p.draw_submenu();
    else if (window == p.app_menu) p.draw_app_menu();
    else if (window == p.notice) p.draw_notice();
    else if (window == p.exit_dialog) p.draw_exit_dialog();
    else if (p.icon_chooser && window == p.icon_chooser->window) p.draw_icon_chooser();
    else if (p.open_with_dialog && window == p.open_with_dialog->window) p.draw_open_with_dialog();
    else if (p.properties && window == p.properties->window) p.draw_properties();
    else if (auto it = p.folders.find(window); it != p.folders.end()) p.draw_folder(it->second);
  } else if (type == XCB_BUTTON_PRESS) {
    auto press = *reinterpret_cast<xcb_button_press_event_t*>(event);
    press.event_x = static_cast<std::int16_t>(std::lround(press.event_x / p.scale));
    press.event_y = static_cast<std::int16_t>(std::lround(press.event_y / p.scale));
    press.root_x = static_cast<std::int16_t>(std::lround(press.root_x / p.scale));
    press.root_y = static_cast<std::int16_t>(std::lround(press.root_y / p.scale));
    p.button(&press);
  }
  else if (type == XCB_BUTTON_RELEASE && p.scroll_drag_window == window) {
    p.scroll_drag_window = 0;
    xcb_ungrab_pointer(p.connection, XCB_CURRENT_TIME);
  } else if (type == XCB_BUTTON_RELEASE && p.drag_window == window) {
    p.drag_window = 0;
    xcb_ungrab_pointer(p.connection, XCB_CURRENT_TIME);
  } else if (type == XCB_BUTTON_RELEASE) p.pending_icon_window = 0;
  else if (type == XCB_MOTION_NOTIFY) {
    auto motion = *reinterpret_cast<xcb_motion_notify_event_t*>(event);
    motion.event_x = static_cast<std::int16_t>(std::lround(motion.event_x / p.scale));
    motion.event_y = static_cast<std::int16_t>(std::lround(motion.event_y / p.scale));
    motion.root_x = static_cast<std::int16_t>(std::lround(motion.root_x / p.scale));
    motion.root_y = static_cast<std::int16_t>(std::lround(motion.root_y / p.scale));
    p.motion(&motion);
  }
  else if (type == XCB_KEY_PRESS) p.key(reinterpret_cast<xcb_key_press_event_t*>(event));
  else if (type == XCB_DESTROY_NOTIFY) {
    p.unwatch(window);
    p.folders.erase(window);
    if (p.focused_folder == window) { p.focused_folder = 0; p.draw_panel(); }
  }
  return true;
}

void Shell::tick() {
  auto& p = *impl_;
  bool redraw = false;
  const auto now = clock_text(p.config.clock_format);
  if (now != p.last_clock) { p.last_clock = now; redraw = true; }
  const auto steady_now = std::chrono::steady_clock::now();
  if (steady_now - p.last_volume_poll >= std::chrono::seconds(1)) {
    p.last_volume_poll = steady_now;
    auto current = p.volume.read();
    std::optional<VolumeState> state = current ? std::optional<VolumeState>(*current) : std::nullopt;
    if (state != p.volume_state) { p.volume_state = state; redraw = true; }
  }
  if (redraw) p.draw_panel();
}

void Shell::set_tasks(std::vector<Task> tasks) {
  impl_->tasks = std::move(tasks);
  if (impl_->panel) { impl_->raise_panel(); impl_->draw_panel(); }
}

xcb_window_t Shell::panel_window() const { return impl_->panel; }

void Shell::set_tray_width(int width) {
  auto& p = *impl_;
  const int requested = std::max(0, width);
  if (p.requested_tray_width == requested) return;
  p.requested_tray_width = requested;
  if (p.panel) p.draw_panel();
}

int Shell::tray_width() const { return impl_->tray_width(); }

int Shell::tray_x() const { return impl_->tray_x(); }
int Shell::panel_x() const { return impl_->primary_monitor.x; }
int Shell::panel_y() const { return impl_->primary_monitor.y; }

const ThemeConfig& Shell::theme() const { return impl_->config; }

const std::array<double, 3>& Shell::panel_color() const {
  return impl_->config.panel_color;
}

cairo_surface_t* Shell::themed_icon_surface(const std::string& name) {
  return impl_->themed_icon_surface(name);
}

const std::array<double, 3>& Shell::accent_color() const {
  return impl_->config.accent_color;
}

void Shell::set_exit_handler(std::function<void()> handler) {
  impl_->exit_handler = std::move(handler);
}

void Shell::set_settings_program(std::string program) {
  impl_->settings_program = std::move(program);
}

void Shell::apply_config(ThemeConfig config) {
  auto& p = *impl_;
  const bool shortcuts_changed = p.config.show_documents != config.show_documents ||
      p.config.show_pictures != config.show_pictures || p.config.show_music != config.show_music ||
      p.config.show_videos != config.show_videos || p.config.show_public_share != config.show_public_share;
  p.config = std::move(config);
  // Clear even when the new selection is empty, and reload a changed image at
  // the same monitor dimensions on the next desktop paint.
  (void)p.wallpaper.load({}, 0, 0);
  p.wallpaper_width = p.wallpaper_height = 0;
  p.hidden_shortcuts.clear();
  p.seed_objects();
  p.desktop_items = p.list_items(Store::desktop_path(), true);
  if (shortcuts_changed) p.selected_desktop = -1;
  refresh_display_layout();
  p.desktop_scroll_rows = std::clamp(p.desktop_scroll_rows, 0, p.desktop_max_scroll());
  p.draw_desktop();
}

void Shell::set_startup_notification(StartupNotification* startup) {
  impl_->startup = startup;
  if (startup) startup->set_change_handler([this](std::size_t) {
    if (impl_->panel) impl_->draw_panel();
  });
}

void Shell::set_screen_size(std::uint16_t width, std::uint16_t height) {
  auto& p = *impl_;
  if (p.physical_width == width && p.physical_height == height) {
    refresh_display_layout();
    return;
  }
  p.physical_width = width;
  p.physical_height = height;
  refresh_display_layout();
}

void Shell::refresh_display_layout() {
  auto& p = *impl_;
  p.close_menu();
  p.update_monitor_geometry();
  p.scale = display_scale(p.primary_monitor.height, p.config.ui_scale);
  p.width = std::max(1, static_cast<int>(p.physical_width / p.scale));
  p.height = std::max(1, static_cast<int>(p.physical_height / p.scale));
  p.update_monitor_geometry();
  const auto primary = p.primary_area();
  auto configure = [&p](xcb_window_t window, int x, int y, int w, int h) {
    if (!window) return;
    const std::uint32_t values[]{
      static_cast<std::uint32_t>(std::lround(x * p.scale)),
      static_cast<std::uint32_t>(std::lround(y * p.scale)),
      static_cast<std::uint32_t>(scaled_pixel(p.scale, w)),
      static_cast<std::uint32_t>(scaled_pixel(p.scale, h))};
    xcb_configure_window(p.connection, window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
  };
  if (p.desktop) {
    configure(p.desktop, 0, 0, p.width, p.height);
    const std::uint32_t size[]{p.physical_width, p.physical_height};
    xcb_configure_window(p.connection, p.desktop,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
    const std::uint32_t below[]{XCB_STACK_MODE_BELOW};
    xcb_configure_window(p.connection, p.desktop, XCB_CONFIG_WINDOW_STACK_MODE, below);
    p.draw_desktop();
  }
  if (p.panel) {
    configure(p.panel, primary.x, primary.y, p.panel_width, Impl::panel_height);
    const std::uint32_t size[]{static_cast<std::uint32_t>(p.primary_monitor.width),
                               static_cast<std::uint32_t>(scaled_pixel(p.scale, Impl::panel_height))};
    xcb_configure_window(p.connection, p.panel,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
    p.draw_panel();
  }
  for (auto& [window, folder] : p.folders) {
    const auto area = p.area_at(folder.x + folder.width / 2, folder.y + folder.height / 2);
    folder.width = std::min(folder.width, std::max(250, area.width - 12));
    const int top = area.y + (area.x == primary.x && area.y == primary.y ? Impl::panel_height : 0);
    folder.height = std::min(folder.height, std::max(180, area.height - (top - area.y) - 12));
    folder.x = std::clamp(folder.x, area.x, std::max(area.x, area.x + area.width - folder.width));
    folder.y = std::clamp(folder.y, top,
                          std::max(top, area.y + area.height - folder.height));
    configure(window, folder.x, folder.y, folder.width, folder.height);
    p.draw_folder(folder);
  }
  auto center = [&p, &configure](xcb_window_t window, int w, int h, const Impl::MonitorArea& area) {
    const auto [x, y] = p.centered(area, w, h);
    configure(window, x, y, w, h);
  };
  center(p.notice, 420, 100, p.owner_area(p.menu_owner));
  center(p.exit_dialog, exit_dialog_width, exit_dialog_height, primary);
  if (p.properties) center(p.properties->window, 500, 220, p.owner_area(p.properties->owner));
  if (p.icon_chooser && p.properties)
    center(p.icon_chooser->window, icon_chooser_width, icon_chooser_height,
           p.owner_area(p.properties->owner));
  if (p.open_with_dialog)
    center(p.open_with_dialog->window, open_with_width, open_with_height,
           p.owner_area(p.open_with_dialog->owner));
  if (p.notice) p.draw_notice();
  if (p.exit_dialog) p.draw_exit_dialog();
  if (p.properties) p.draw_properties();
  if (p.icon_chooser) p.draw_icon_chooser();
  if (p.open_with_dialog) p.draw_open_with_dialog();
  xcb_flush(p.connection);
}

double Shell::ui_scale() const { return impl_->scale; }
int Shell::panel_height() const { return scaled_pixel(impl_->scale, Impl::panel_height); }
int Shell::screen_width() const { return impl_->physical_width; }
int Shell::screen_height() const { return impl_->physical_height; }

void Shell::set_file_clipboard_handlers(
    std::function<void(const std::vector<std::filesystem::path>&, bool)> set,
    std::function<void(std::function<void(std::vector<std::filesystem::path>, bool)>)> request) {
  impl_->clipboard_set = std::move(set);
  impl_->clipboard_request = std::move(request);
}

bool Shell::show_folder(const std::filesystem::path& path,
                        const std::optional<std::filesystem::path>& selected) {
  return impl_->open_folder(path, selected);
}

bool Shell::show_item_properties(const std::filesystem::path& path) {
  auto& p = *impl_;
  if (path.empty() || path == "/") return false;
  const auto items = p.list_items(path.parent_path(), false);
  const auto normalized = path.lexically_normal();
  const auto it = std::ranges::find_if(items, [&](const Item& item) {
    return item.path.lexically_normal() == normalized;
  });
  if (it == items.end()) return false;
  p.close_menu();
  p.menu_owner = 0;
  p.show_properties(*it);
  return true;
}

void Shell::set_dnd_handlers(std::function<void(xcb_window_t, DropHandler)> register_target,
                             std::function<void(xcb_window_t)> unregister_target,
                             std::function<void(xcb_window_t, std::vector<std::filesystem::path>, bool)> begin_drag) {
  impl_->dnd_register = std::move(register_target);
  impl_->dnd_unregister = std::move(unregister_target);
  impl_->dnd_begin = std::move(begin_drag);
  if (impl_->desktop) impl_->register_drop(impl_->desktop);
  for (auto& [window, folder] : impl_->folders) impl_->register_drop(window);
}

} // namespace midnight
