#include "shell.hpp"

#include "config.hpp"
#include "model.hpp"
#include "store.hpp"

#include <cairo/cairo-xcb.h>
#include <gio/gio.h>
#include <pango/pangocairo.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace midnight {
namespace {
constexpr int panel_height = 34;
constexpr int row_height = 30;
constexpr int icon_width = 90;
constexpr int icon_height = 90;
constexpr int folder_header = 55;

using Item = ShellItem;

struct Folder {
  xcb_window_t window{};
  std::filesystem::path path;
  int x{}, y{}, width{620}, height{430};
  int selected{-1};
  int scroll_rows{};
  std::vector<Item> items;
  bool trash{};
};

struct Properties {
  xcb_window_t window{};
  Item item;
  xcb_window_t owner{};
  std::array<std::string, 3> values;
  int field{};
};

struct TransferWork {
  std::vector<std::filesystem::path> paths;
  std::filesystem::path destination;
  bool move{};
};

struct TransferCompletion {
  std::shared_ptr<bool> alive;
  std::function<void(std::expected<void, std::string>)> callback;
};

enum class MenuAction { open_home, open_desktop, new_folder, paste, launch, next_apps, previous_apps,
                        open_item, copy, cut, create_shadow, create_template, properties, trash,
                        restore, delete_permanently, close_folder };
struct MenuEntry { std::string label; MenuAction action; std::size_t index{}; };

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

void bevel(cairo_t* cr, int x, int y, int w, int h, bool sunken = false) {
  cairo_set_line_width(cr, 1);
  cairo_set_source_rgb(cr, sunken ? .25 : 1, sunken ? .25 : 1, sunken ? .25 : 1);
  cairo_move_to(cr, x + w - 1, y);
  cairo_line_to(cr, x, y);
  cairo_line_to(cr, x, y + h - 1);
  cairo_stroke(cr);
  cairo_set_source_rgb(cr, sunken ? 1 : .25, sunken ? 1 : .25, sunken ? 1 : .25);
  cairo_move_to(cr, x + w - 1, y);
  cairo_line_to(cr, x + w - 1, y + h - 1);
  cairo_line_to(cr, x, y + h - 1);
  cairo_stroke(cr);
}

void text(cairo_t* cr, const std::string& value, int x, int y, int width,
          int size = 11, bool white = false, bool bold = false) {
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_from_string(bold ? "Sans Bold" : "Sans");
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, value.c_str(), -1);
  pango_layout_set_width(layout, width * PANGO_SCALE);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  cairo_set_source_rgb(cr, white ? 1 : .04, white ? 1 : .04, white ? 1 : .04);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(font);
  g_object_unref(layout);
}

void icon(cairo_t* cr, const Item& item, int x, int y) {
  if (!item.object.icon.empty() && std::filesystem::path(item.object.icon).is_absolute()) {
    auto* surface = cairo_image_surface_create_from_png(item.object.icon.c_str());
    if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
      const auto source_width = cairo_image_surface_get_width(surface);
      const auto source_height = cairo_image_surface_get_height(surface);
      if (source_width > 0 && source_height > 0) {
        const auto scale = std::min(40.0 / source_width, 40.0 / source_height);
        cairo_save(cr);
        cairo_translate(cr, x + 4 + (40 - source_width * scale) / 2,
                            y + 3 + (40 - source_height * scale) / 2);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(surface);
        return;
      }
    }
    cairo_surface_destroy(surface);
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
    bevel(cr, x + 5, y + 12, 38, 28);
  } else {
    fill(cr, .94, .94, .91, x + 10, y + 4, 29, 37);
    bevel(cr, x + 10, y + 4, 29, 37);
    fill(cr, .20, .36, .65, x + 15, y + 13, 18, 3);
    fill(cr, .40, .40, .43, x + 15, y + 22, 17, 2);
    fill(cr, .40, .40, .43, x + 15, y + 28, 14, 2);
  }
  if (item.object.kind == ObjectKind::shadow) {
    fill(cr, .05, .10, .45, x + 28, y + 29, 14, 13);
    text(cr, "↗", x + 29, y + 26, 12, 12, true);
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
  xcb_connection_t* connection;
  xcb_screen_t* screen;
  Store& store;
  ShellModel model;
  ThemeConfig config{load_config()};
  std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
  int transfers_in_progress{};
  xcb_visualtype_t* visual{};
  xcb_window_t desktop{}, panel{}, menu{};
  xcb_window_t notice{};
  std::string notice_text;
  std::optional<Properties> properties;
  std::uint16_t width{}, height{};
  std::unordered_map<xcb_window_t, Folder> folders;
  std::unordered_map<xcb_window_t, GFileMonitor*> monitors;
  std::vector<Item> desktop_items;
  std::vector<Task> tasks;
  std::vector<GAppInfo*> apps;
  std::vector<MenuEntry> menu_entries;
  std::size_t app_page{};
  xcb_window_t menu_owner{};
  int menu_x{}, menu_y{}, menu_width{250};
  int selected_desktop{-1};
  std::optional<Item> selected_item;
  std::filesystem::path clipboard_path;
  bool clipboard_cut{};
  std::function<void(const std::vector<std::filesystem::path>&, bool)> clipboard_set;
  std::function<void(std::function<void(std::vector<std::filesystem::path>, bool)>)> clipboard_request;
  std::function<void(xcb_window_t, Shell::DropHandler)> dnd_register;
  std::function<void(xcb_window_t)> dnd_unregister;
  std::function<void(xcb_window_t, std::vector<std::filesystem::path>, bool)> dnd_begin;
  xcb_key_symbols_t* key_symbols{};
  xcb_atom_t active_atom{};
  std::string last_clock;
  xcb_timestamp_t last_click_time{};
  int last_click_index{-1};
  xcb_window_t last_click_window{};
  xcb_window_t drag_window{};
  int drag_root_x{}, drag_root_y{}, drag_start_x{}, drag_start_y{};
  bool resizing{};
  xcb_window_t pending_icon_window{};
  int pending_icon_index{-1};
  int pending_icon_x{}, pending_icon_y{};

  Impl(xcb_connection_t* c, xcb_screen_t* s, Store& st) : connection(c), screen(s), store(st), model(st), width(s->width_in_pixels), height(s->height_in_pixels) {}
  ~Impl() {
    *alive = false;
    if (key_symbols) xcb_key_symbols_free(key_symbols);
    for (auto* app : apps) g_object_unref(app);
    for (auto& [window, monitor] : monitors) g_object_unref(monitor);
    for (auto& [window, folder] : folders) xcb_destroy_window(connection, window);
    if (menu) xcb_destroy_window(connection, menu);
    if (notice) xcb_destroy_window(connection, notice);
    if (properties) xcb_destroy_window(connection, properties->window);
    if (panel) xcb_destroy_window(connection, panel);
    if (desktop) xcb_destroy_window(connection, desktop);
  }

  xcb_window_t create_window(int x, int y, int w, int h, std::uint32_t extra_mask = 0) {
    auto id = xcb_generate_id(connection);
    const std::uint32_t values[] = {screen->black_pixel, 1,
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
      XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY | extra_mask};
    xcb_create_window(connection, screen->root_depth, id, screen->root, x, y, w, h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
    return id;
  }

  void paint(xcb_window_t window, int w, int h, auto&& draw) {
    if (!visual || w <= 0 || h <= 0) return;
    auto* surface = cairo_xcb_surface_create(connection, window, visual, w, h);
    auto* cr = cairo_create(surface);
    draw(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    xcb_flush(connection);
  }

  void reload_apps() {
    for (auto* app : apps) g_object_unref(app);
    apps.clear();
    GList* list = g_app_info_get_all();
    for (GList* cursor = list; cursor; cursor = cursor->next) {
      auto* app = G_APP_INFO(cursor->data);
      if (g_app_info_should_show(app)) apps.push_back(G_APP_INFO(g_object_ref(app)));
    }
    g_list_free_full(list, g_object_unref);
    std::ranges::sort(apps, [](GAppInfo* a, GAppInfo* b) {
      return g_utf8_collate(g_app_info_get_display_name(a), g_app_info_get_display_name(b)) < 0;
    });
  }

  void seed_objects() {
    const auto home = std::filesystem::path(g_get_home_dir());
    auto ensure = [&](std::string id, ObjectKind kind, std::string title, std::string target, int x, int y) {
      if (store.get(id)) return;
      (void)store.put({std::move(id), kind, std::move(title), std::move(target), "", "", x, y});
    };
    ensure("home", ObjectKind::folder, "Home", home.string(), 12, 15);
    ensure("trash", ObjectKind::trash, "Trash", "trash:///", 12, 112);
    ensure("templates", ObjectKind::folder, "Templates", (home / "Templates").string(), 12, 209);
  }

  std::vector<Item> list_items(const std::filesystem::path& path, bool is_desktop) {
    return model.items(path, is_desktop);
  }

  void refresh() {
    desktop_items = list_items(Store::desktop_path(), true);
    for (auto& [window, folder] : folders) {
      folder.items = list_items(folder.path, false);
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
    paint(panel, width, panel_height, [&](cairo_t* cr) {
      fill(cr, config.panel_color[0], config.panel_color[1], config.panel_color[2], 0, 0, width, panel_height);
      fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2], 0, 0, width, 3);
      fill(cr, config.accent_color[0], config.accent_color[1], config.accent_color[2], 4, 5, 116, 24);
      bevel(cr, 4, 5, 116, 24);
      text(cr, config.panel_label, 13, 8, 102, 12, true, true);
      int x = 128;
      const int clock_x = std::max(140, static_cast<int>(width) - 105);
      if (transfers_in_progress) text(cr, "Working…", clock_x - 85, 8, 80, 11);
      for (const auto& task : panel_tasks()) {
        if (x + 105 > clock_x) break;
        fill(cr, task.active ? .28 : .80, task.active ? .39 : .80,
             task.active ? .63 : .80, x, 5, 110, 24);
        bevel(cr, x, 5, 110, 24, task.active);
        text(cr, task.title, x + 7, 8, 95, 11, task.active);
        x += 114;
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
      result.push_back({window, folder.trash ? "Trash" : basename(folder.path), false});
    }
    return result;
  }

  void raise_panel() {
    const std::uint32_t above[]{XCB_STACK_MODE_ABOVE};
    if (panel) xcb_configure_window(connection, panel, XCB_CONFIG_WINDOW_STACK_MODE, above);
    if (menu) xcb_configure_window(connection, menu, XCB_CONFIG_WINDOW_STACK_MODE, above);
    xcb_flush(connection);
  }

  void draw_items(cairo_t* cr, const std::vector<Item>& items, int start_y, int width_px,
                  int selected, bool on_desktop, int scroll_rows = 0) {
    const int cols = std::max(1, width_px / icon_width);
    for (std::size_t i = static_cast<std::size_t>(scroll_rows * cols); i < items.size(); ++i) {
      const int x = 12 + static_cast<int>(i % cols) * icon_width;
      const int y = start_y + (static_cast<int>(i / cols) - scroll_rows) * icon_height;
      if (selected == static_cast<int>(i)) fill(cr, .19, .32, .57, x, y, 75, 74);
      icon(cr, items[i], x + 14, y + 3);
      text(cr, items[i].object.title, x + 2, y + 50, 76, 10,
           selected == static_cast<int>(i) || on_desktop);
    }
  }

  void draw_desktop() {
    paint(desktop, width, std::max(1, static_cast<int>(height) - panel_height), [&](cairo_t* cr) {
      fill(cr, config.desktop_color[0], config.desktop_color[1], config.desktop_color[2], 0, 0, width, height - panel_height);
      draw_items(cr, desktop_items, 13, width, selected_desktop, true);
    });
  }

  void draw_folder(Folder& folder) {
    paint(folder.window, folder.width, folder.height, [&](cairo_t* cr) {
      fill(cr, .73, .73, .73, 0, 0, folder.width, folder.height);
      bevel(cr, 0, 0, folder.width, folder.height);
      fill(cr, .14, .27, .56, 4, 4, folder.width - 8, 26);
      text(cr, folder.trash ? "Trash" : basename(folder.path), 12, 7, folder.width - 65, 13, true, true);
      fill(cr, .73, .73, .73, folder.width - 30, 7, 20, 19);
      bevel(cr, folder.width - 30, 7, 20, 19);
      text(cr, "×", folder.width - 25, 6, 17, 15);
      text(cr, "File    Edit    View", 12, 34, folder.width - 24, 11);
      fill(cr, .96, .96, .92, 5, folder_header, folder.width - 10, folder.height - folder_header - 22);
      cairo_save(cr);
      cairo_rectangle(cr, 5, folder_header, folder.width - 10, folder.height - folder_header - 22);
      cairo_clip(cr);
      draw_items(cr, folder.items, folder_header + 11, folder.width - 10, folder.selected, false, folder.scroll_rows);
      cairo_restore(cr);
      fill(cr, .73, .73, .73, 5, folder.height - 21, folder.width - 10, 16);
      text(cr, std::to_string(folder.items.size()) + " objects", 11, folder.height - 21, folder.width - 22, 10);
    });
  }

  void draw_menu() {
    if (!menu) return;
    const int h = static_cast<int>(menu_entries.size()) * row_height + 8;
    paint(menu, menu_width, h, [&](cairo_t* cr) {
      fill(cr, .73, .73, .73, 0, 0, menu_width, h);
      bevel(cr, 0, 0, menu_width, h);
      for (std::size_t i = 0; i < menu_entries.size(); ++i) {
        if (i % 2 == 0) fill(cr, .78, .78, .78, 4, 4 + static_cast<int>(i) * row_height, menu_width - 8, row_height);
        text(cr, menu_entries[i].label, 14, 9 + static_cast<int>(i) * row_height, menu_width - 24, 11);
      }
    });
  }

  void show_notice(std::string message) {
    notice_text = std::move(message);
    if (notice) xcb_destroy_window(connection, notice);
    const int x = std::max(0, (static_cast<int>(width) - 420) / 2);
    const int y = std::max(panel_height, (static_cast<int>(height) - 100) / 2);
    notice = create_window(x, y, 420, 100);
    xcb_map_window(connection, notice);
    draw_notice();
  }

  void show_properties(const Item& item) {
    if (properties) xcb_destroy_window(connection, properties->window);
    Properties dialog;
    dialog.item = item;
    dialog.owner = menu_owner;
    dialog.values = {item.object.title, item.object.icon, item.object.target};
    dialog.window = create_window(std::max(0, (static_cast<int>(width) - 500) / 2),
                                  std::max(panel_height, (static_cast<int>(height) - 220) / 2), 500, 220);
    properties = std::move(dialog);
    xcb_map_window(connection, properties->window);
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, properties->window, XCB_CURRENT_TIME);
    draw_properties();
  }

  void draw_properties() {
    if (!properties) return;
    paint(properties->window, 500, 220, [&](cairo_t* cr) {
      fill(cr, .75, .75, .75, 0, 0, 500, 220);
      bevel(cr, 0, 0, 500, 220);
      fill(cr, .14, .27, .56, 4, 4, 492, 27);
      text(cr, "Properties", 13, 8, 450, 12, true, true);
      constexpr std::array<std::string_view, 3> labels{"Name", "Icon", "Target"};
      for (int i = 0; i < 3; ++i) {
        const int y = 47 + i * 45;
        text(cr, std::string(labels[i]), 14, y + 5, 80, 11);
        fill(cr, 1, 1, 1, 92, y, 390, 29);
        bevel(cr, 92, y, 390, 29, true);
        text(cr, properties->values[i] + (i == properties->field ? "|" : ""), 99, y + 4, 375, 11);
      }
      bevel(cr, 316, 184, 75, 27);
      bevel(cr, 400, 184, 75, 27);
      text(cr, "Save", 338, 188, 52, 11);
      text(cr, "Cancel", 416, 188, 55, 11);
    });
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
      fill(cr, .75, .75, .75, 0, 0, 420, 100);
      bevel(cr, 0, 0, 420, 100);
      fill(cr, .14, .27, .56, 4, 4, 412, 25);
      text(cr, "Workspace", 12, 7, 390, 12, true, true);
      text(cr, notice_text, 14, 42, 390, 11);
      bevel(cr, 170, 70, 80, 22);
      text(cr, "OK", 199, 71, 50, 11);
    });
  }

  void close_menu() {
    if (!menu) return;
    xcb_destroy_window(connection, menu);
    menu = 0;
    menu_entries.clear();
    xcb_flush(connection);
  }

  void show_menu(int x, int y, xcb_window_t owner, std::vector<MenuEntry> entries) {
    close_menu();
    menu_entries = std::move(entries);
    if (menu_entries.empty()) return;
    menu_owner = owner;
    const int h = static_cast<int>(menu_entries.size()) * row_height + 8;
    menu_x = std::clamp(x, 0, std::max(0, static_cast<int>(width) - menu_width));
    menu_y = std::clamp(y, 0, std::max(0, static_cast<int>(height) - h));
    menu = create_window(menu_x, menu_y, menu_width, h);
    xcb_map_window(connection, menu);
    raise_panel();
    draw_menu();
  }

  void show_launcher(std::size_t page = 0) {
    reload_apps();
    std::vector<MenuEntry> entries{{"Home", MenuAction::open_home}, {"Desktop", MenuAction::open_desktop},
                                   {"New folder on desktop", MenuAction::new_folder}, {"Paste on desktop", MenuAction::paste}};
    const auto page_size = static_cast<std::size_t>(std::max<int>(1, (height - 220) / row_height));
    app_page = std::min(page, apps.empty() ? 0 : (apps.size() - 1) / page_size);
    if (app_page) entries.push_back({"◀ Previous applications", MenuAction::previous_apps});
    const auto start = app_page * page_size;
    for (std::size_t i = start; i < std::min(apps.size(), start + page_size); ++i)
      entries.push_back({g_app_info_get_display_name(apps[i]), MenuAction::launch, i});
    if (start + page_size < apps.size()) entries.push_back({"More applications ▶", MenuAction::next_apps});
    show_menu(3, panel_height, desktop, std::move(entries));
  }

  void show_context(xcb_window_t window, int root_x, int root_y, const Item* item) {
    if (item) selected_item = *item;
    else selected_item.reset();
    std::vector<MenuEntry> entries;
    if (item && item->trashed) {
      entries = {{"Restore", MenuAction::restore}, {"Delete permanently", MenuAction::delete_permanently},
                 {"Properties", MenuAction::properties}};
    } else if (item) {
      entries = {{"Open", MenuAction::open_item}, {"Copy", MenuAction::copy}, {"Cut", MenuAction::cut},
                 {"Create shadow", MenuAction::create_shadow}, {"Create template", MenuAction::create_template},
                 {"Properties", MenuAction::properties}, {"Move to Trash", MenuAction::trash}};
    } else if (!(folders.contains(window) && folders.at(window).trash)) {
      entries = {{"New folder", MenuAction::new_folder}, {"Paste", MenuAction::paste}};
    }
    show_menu(root_x, root_y, window, std::move(entries));
  }

  std::filesystem::path owner_path() const {
    if (const auto it = folders.find(menu_owner); it != folders.end()) return it->second.path;
    return Store::desktop_path();
  }

  void open_folder(const std::filesystem::path& path) {
    std::error_code ec;
    const bool trash = path == "trash:///";
    if (!trash && !std::filesystem::is_directory(path, ec)) { show_notice("Folder unavailable: " + path.string()); return; }
    Folder folder;
    folder.path = path;
    folder.trash = trash;
    folder.x = 85 + 24 * static_cast<int>(folders.size() % 10);
    folder.y = 75 + 22 * static_cast<int>(folders.size() % 10);
    folder.width = std::min<int>(620, width - 30);
    folder.height = std::min<int>(430, height - 60);
    folder.window = create_window(folder.x, folder.y, folder.width, folder.height);
    folder.items = list_items(path, false);
    xcb_map_window(connection, folder.window);
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, folder.window, XCB_CURRENT_TIME);
    auto [it, inserted] = folders.emplace(folder.window, std::move(folder));
    if (inserted) { watch_directory(it->first, path); register_drop(it->first); draw_folder(it->second); draw_panel(); }
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
    const auto uri = file_uri(path);
    GError* error = nullptr;
    g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error);
    if (error) {
      show_notice(std::string("Open failed: ") + error->message);
      g_error_free(error);
    }
  }

  void execute(MenuEntry entry) {
    const auto destination = owner_path();
    close_menu();
    switch (entry.action) {
      case MenuAction::open_home: open_folder(g_get_home_dir()); break;
      case MenuAction::open_desktop: open_folder(Store::desktop_path()); break;
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
        g_app_info_launch(apps[entry.index], nullptr, nullptr, &error);
        if (error) {
          show_notice(std::string("Launch failed: ") + error->message);
          g_error_free(error);
        }
        break;
      }
      case MenuAction::next_apps: show_launcher(app_page + 1); break;
      case MenuAction::previous_apps: show_launcher(app_page ? app_page - 1 : 0); break;
      case MenuAction::open_item: if (selected_item) open_item(*selected_item); break;
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
      case MenuAction::close_folder: break;
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
    if (properties && event->event == properties->window) {
      if (event->event_y >= 184 && event->event_y <= 211) {
        if (event->event_x >= 316 && event->event_x <= 391) save_properties();
        else if (event->event_x >= 400 && event->event_x <= 475) {
          xcb_destroy_window(connection, properties->window);
          properties.reset();
        }
      } else if (event->event_y >= 47 && event->event_y < 166) {
        const int field = (event->event_y - 47) / 45;
        if (!(properties->item.physical && field == 2)) properties->field = field;
        draw_properties();
      }
      return;
    }
    if (event->event == notice) {
      xcb_destroy_window(connection, notice);
      notice = 0;
      return;
    }
    if (event->event == menu) {
      const int index = (event->event_y - 4) / row_height;
      if (event->detail == 1 && event->event_y >= 4 && index >= 0 && static_cast<std::size_t>(index) < menu_entries.size())
        execute(menu_entries[index]);
      else close_menu();
      return;
    }
    if (event->event == panel) {
      if (event->detail == 1 && event->event_x < 122) { if (menu) close_menu(); else show_launcher(); }
      else if (event->detail == 1 && event->event_x >= 128) {
        const int index = (event->event_x - 128) / 114;
        const auto all_tasks = panel_tasks();
        if (index >= 0 && static_cast<std::size_t>(index) < all_tasks.size()) {
          if (folders.contains(all_tasks[index].window)) {
            const std::uint32_t above[]{XCB_STACK_MODE_ABOVE};
            xcb_configure_window(connection, all_tasks[index].window, XCB_CONFIG_WINDOW_STACK_MODE, above);
            xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT, all_tasks[index].window, event->time);
            raise_panel();
            return;
          }
          xcb_client_message_event_t request{};
          request.response_type = XCB_CLIENT_MESSAGE;
          request.format = 32;
          request.window = all_tasks[index].window;
          request.type = active_atom;
          request.data.data32[0] = 2;
          request.data.data32[1] = event->time;
          xcb_send_event(connection, 0, screen->root,
                         XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                         reinterpret_cast<const char*>(&request));
          xcb_flush(connection);
        }
      }
      return;
    }
    if (menu) close_menu();
    auto* folder = [&]() -> Folder* { auto it = folders.find(event->event); return it == folders.end() ? nullptr : &it->second; }();
    if (folder && (event->detail == 4 || event->detail == 5)) {
      const int cols = std::max(1, (folder->width - 10) / icon_width);
      const int rows = static_cast<int>((folder->items.size() + cols - 1) / cols);
      const int visible = std::max(1, (folder->height - folder_header - 22) / icon_height);
      folder->scroll_rows = std::clamp(folder->scroll_rows + (event->detail == 5 ? 1 : -1), 0,
                                       std::max(0, rows - visible));
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
    const bool on_desktop = event->event == desktop;
    if (!on_desktop && !folder) return;
    auto& items = on_desktop ? desktop_items : folder->items;
    const int start_y = on_desktop ? 13 : folder_header + 11;
    const int width_px = on_desktop ? width : folder->width - 10;
    const int index = item_index(event->event_x, event->event_y, start_y, width_px, items.size(),
                                 folder ? folder->scroll_rows : 0);
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
    if (properties && event->event == properties->window) {
      const auto sym = key_symbols ? xcb_key_symbols_get_keysym(key_symbols, event->detail,
                                                       (event->state & XCB_MOD_MASK_SHIFT) ? 1 : 0) : 0;
      if (sym == XK_Escape) { xcb_destroy_window(connection, properties->window); properties.reset(); return; }
      if (sym == XK_Return) { save_properties(); return; }
      if (sym == XK_Tab) {
        properties->field = (properties->field + 1) % (properties->item.physical ? 2 : 3);
      } else if (sym == XK_BackSpace) {
        auto& value = properties->values[properties->field];
        if (!value.empty()) value.pop_back();
      } else if (!(event->state & XCB_MOD_MASK_CONTROL) && sym >= 0x20 && sym <= 0x7e) {
        properties->values[properties->field] += static_cast<char>(sym);
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
      const std::uint32_t values[]{static_cast<std::uint32_t>(folder.width), static_cast<std::uint32_t>(folder.height)};
      xcb_configure_window(connection, folder.window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
      draw_folder(folder);
    } else {
      folder.x = drag_start_x + dx;
      folder.y = drag_start_y + dy;
      const std::uint32_t values[]{static_cast<std::uint32_t>(folder.x), static_cast<std::uint32_t>(folder.y)};
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
  p.key_symbols = xcb_key_symbols_alloc(p.connection);
  auto atom_cookie = xcb_intern_atom(p.connection, 0, 18, "_NET_ACTIVE_WINDOW");
  if (auto* reply = xcb_intern_atom_reply(p.connection, atom_cookie, nullptr)) {
    p.active_atom = reply->atom;
    std::free(reply);
  }
  std::error_code ec;
  std::filesystem::create_directories(Store::desktop_path(), ec);
  p.seed_objects();
  p.desktop = p.create_window(0, panel_height, p.width, std::max(1, static_cast<int>(p.height) - panel_height));
  p.panel = p.create_window(0, 0, p.width, panel_height);
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
  return window && (window == p.desktop || window == p.panel || window == p.menu || window == p.notice ||
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
    case XCB_DESTROY_NOTIFY: window = reinterpret_cast<xcb_destroy_notify_event_t*>(event)->window; break;
    default: return false;
  }
  if (!owns(window)) return false;
  if (type == XCB_EXPOSE) {
    if (window == p.panel) p.draw_panel();
    else if (window == p.desktop) p.draw_desktop();
    else if (window == p.menu) p.draw_menu();
    else if (window == p.notice) p.draw_notice();
    else if (p.properties && window == p.properties->window) p.draw_properties();
    else if (auto it = p.folders.find(window); it != p.folders.end()) p.draw_folder(it->second);
  } else if (type == XCB_BUTTON_PRESS) p.button(reinterpret_cast<xcb_button_press_event_t*>(event));
  else if (type == XCB_BUTTON_RELEASE && p.drag_window == window) {
    p.drag_window = 0;
    xcb_ungrab_pointer(p.connection, XCB_CURRENT_TIME);
  } else if (type == XCB_BUTTON_RELEASE) p.pending_icon_window = 0;
  else if (type == XCB_MOTION_NOTIFY) p.motion(reinterpret_cast<xcb_motion_notify_event_t*>(event));
  else if (type == XCB_KEY_PRESS) p.key(reinterpret_cast<xcb_key_press_event_t*>(event));
  else if (type == XCB_DESTROY_NOTIFY) { p.unwatch(window); p.folders.erase(window); }
  return true;
}

void Shell::tick() {
  auto& p = *impl_;
  const auto now = clock_text(p.config.clock_format);
  if (now != p.last_clock) { p.last_clock = now; p.draw_panel(); }
}

void Shell::set_tasks(std::vector<Task> tasks) {
  impl_->tasks = std::move(tasks);
  if (impl_->panel) { impl_->raise_panel(); impl_->draw_panel(); }
}

void Shell::set_screen_size(std::uint16_t width, std::uint16_t height) {
  auto& p = *impl_;
  p.width = width;
  p.height = height;
  if (p.desktop) {
    const std::uint32_t values[]{width, static_cast<std::uint32_t>(std::max(1, static_cast<int>(height) - panel_height))};
    xcb_configure_window(p.connection, p.desktop, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    const std::uint32_t below[]{XCB_STACK_MODE_BELOW};
    xcb_configure_window(p.connection, p.desktop, XCB_CONFIG_WINDOW_STACK_MODE, below);
    p.draw_desktop();
  }
  if (p.panel) {
    const std::uint32_t values[]{width};
    xcb_configure_window(p.connection, p.panel, XCB_CONFIG_WINDOW_WIDTH, values);
    p.draw_panel();
  }
}

void Shell::set_file_clipboard_handlers(
    std::function<void(const std::vector<std::filesystem::path>&, bool)> set,
    std::function<void(std::function<void(std::vector<std::filesystem::path>, bool)>)> request) {
  impl_->clipboard_set = std::move(set);
  impl_->clipboard_request = std::move(request);
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
