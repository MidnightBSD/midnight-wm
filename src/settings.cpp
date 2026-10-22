// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include "config.hpp"
#include "session.hpp"
#include "application_session.hpp"
#include "application_defaults.hpp"
#include "startup_applications.hpp"

#include <gtk/gtk.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef MIDNIGHT_WM_VERSION
#define MIDNIGHT_WM_VERSION "dev"
#endif
#ifndef MIDNIGHT_WM_BACKGROUND_DIR
#define MIDNIGHT_WM_BACKGROUND_DIR "/usr/local/share/backgrounds/midnightbsd"
#endif

namespace {
struct KeyFileDeleter {
  void operator()(GKeyFile* file) const { g_key_file_unref(file); }
};
using KeyFile = std::unique_ptr<GKeyFile, KeyFileDeleter>;
struct Binding {
  bool changed{};
  std::function<void(GKeyFile*)> save;
};

class Settings {
 public:
  Settings() : path_(std::filesystem::path(g_get_user_config_dir()) / "midnight-wm/config.ini"),
               file_(g_key_file_new()) {
    std::string error;
    valid_ = read_file(original_, existed_, error);
    if (valid_ && existed_) {
      GError* parse_error = nullptr;
      valid_ = g_key_file_load_from_data(file_.get(), original_.data(), original_.size(),
          static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS), &parse_error);
      if (!valid_) { error = parse_error->message; g_error_free(parse_error); }
    }
    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window_), "Midnight Workspace Settings");
    gtk_window_set_icon_name(GTK_WINDOW(window_), "preferences-desktop");
    gtk_window_set_default_size(GTK_WINDOW(window_), 570, 600);
    gtk_container_set_border_width(GTK_CONTAINER(window_), 12);
    g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { gtk_main_quit(); }), nullptr);
    auto* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(window_), outer);
    auto* notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(outer), notebook, TRUE, TRUE, 0);
    auto* appearance = page(notebook, "Appearance");
    row_ = 0;
    checkbox(appearance, "_Dark appearance (workspace and GTK apps)", "Workspace", "dark_mode", false);
    checkbox(appearance, "Window _shadows", "Workspace", "compositing", true);
    color(appearance, "Desktop color", "desktop_color", "#7866a8");
    color(appearance, "Window accent", "accent_color", "#3b205f");
    panel_color(appearance);
    font(appearance, "Interface font", "ui_font", "Sans");
    font(appearance, "Window title font", "title_font", "Sans Bold");
    scaling(appearance);
    wallpaper(appearance);
    auto* mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode), "fill", "Fill (crop to screen)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode), "fit", "Fit (show whole image)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode), string("Workspace", "desktop_image_mode", "fill").c_str());
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(mode)) < 0) gtk_combo_box_set_active(GTK_COMBO_BOX(mode), 0);
    add(appearance, "Image sizing", mode);
    bind(mode, "changed", [mode](GKeyFile* file) {
      g_key_file_set_string(file, "Workspace", "desktop_image_mode", gtk_combo_box_get_active_id(GTK_COMBO_BOX(mode)));
    });
    checkbox(appearance, "_Reduce motion (show menus immediately)", "Workspace", "reduced_motion", false);
    auto* border_style = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(border_style), "classic", "Classic bevel");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(border_style), "two-color", "Two-color outline");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(border_style),
        string("Workspace", "window_border_style", "classic").c_str());
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(border_style)) < 0)
      gtk_combo_box_set_active(GTK_COMBO_BOX(border_style), 0);
    add(appearance, "Window _border style", border_style);
    bind(border_style, "changed", [border_style](GKeyFile* file) {
      g_key_file_set_string(file, "Workspace", "window_border_style",
          gtk_combo_box_get_active_id(GTK_COMBO_BOX(border_style)));
    });
    color(appearance, "_Outer border color", "window_border_outer_color", "#202020");
    color(appearance, "In_ner border color", "window_border_inner_color", "#ffffff");
    auto* workspace = page(notebook, "Desktop and panel");
    row_ = 0;
    entry(workspace, "_Menu label", "panel_label", "Midnight ▾");
    entry(workspace, "_Clock format", "clock_format", "%a %H:%M");
    auto* hint = gtk_label_new("Clock examples: %H:%M for 24-hour time; %I:%M %p for 12-hour time.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(hint), 55);
    gtk_grid_attach(GTK_GRID(workspace), hint, 0, row_++, 2, 1);
    checkbox(workspace, "Show _battery indicator", "Workspace", "show_battery", true);
    checkbox(workspace, "Show Documents", "Desktop", "show_documents", true);
    checkbox(workspace, "Show Pictures", "Desktop", "show_pictures", true);
    checkbox(workspace, "Show Music", "Desktop", "show_music", false);
    checkbox(workspace, "Show Videos", "Desktop", "show_videos", false);
    checkbox(workspace, "Show Public Share", "Desktop", "show_public_share", false);
    auto* defaults = page(notebook, "Application defaults");
    row_ = 0;
    application_defaults(defaults);
    auto* startup = page(notebook, "Startup applications");
    row_ = 0;
    startup_applications_page(startup);
    auto* shortcuts = page(notebook, "Keyboard shortcuts");
    row_ = 0;
    shortcut_page(shortcuts);
    auto* session = page(notebook, "Session");
    row_ = 0;
    checkbox(session, "_Restore folders at login", "Session", "restore_folders", false);
    checkbox(session, "_Save folders on logout", "Session", "save_on_logout", false);
    checkbox(session, "Restore applications at _login", "Session", "restore_applications", false);
    checkbox(session, "Also reopen identified apps _without session support", "Session", "restore_desktop_applications", false);
    checkbox(session, "Save application chec_kpoints", "Session", "save_application_checkpoints", false);
    checkbox(session, "Start _XScreenSaver at login (if installed)", "Session", "start_xscreensaver", true);
    auto* session_hint = gtk_label_new(
      "Disable both restore options to start fresh at the next login.\n"
      "Restores participating apps through the Midnight session companion.\n"
      "Fallback starts one copy; document restoration depends on the app.\n"
      "Saving changes apply after Apply; login choices apply at next login.");
    gtk_label_set_line_wrap(GTK_LABEL(session_hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(session_hint), 50);
    gtk_label_set_xalign(GTK_LABEL(session_hint), 0);
    gtk_grid_attach(GTK_GRID(session), session_hint, 0, row_++, 2, 1);
    auto* clear = gtk_button_new_with_mnemonic("Clear saved _folder session");
    gtk_grid_attach(GTK_GRID(session), clear, 0, row_++, 2, 1);
    g_signal_connect(clear, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      auto* self = static_cast<Settings*>(data);
      const auto result = midnight::SessionStore{}.clear();
      gtk_label_set_text(GTK_LABEL(self->status_), result ?
        "Saved folder session cleared. Saving on logout may create a new snapshot." : result.error().c_str());
    }), this);
    auto* clear_applications = gtk_button_new_with_mnemonic("Clear saved application s_ession");
    gtk_grid_attach(GTK_GRID(session), clear_applications, 0, row_++, 2, 1);
    g_signal_connect(clear_applications, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      auto* self = static_cast<Settings*>(data);
      const auto result = midnight::ApplicationSessionStore{}.clear();
      gtk_label_set_text(GTK_LABEL(self->status_), result ?
        "Saved application session cleared. Running apps stay open; enabled saving may create another snapshot." :
        result.error().c_str());
    }), this);
    status_ = gtk_label_new("Apply saves your settings and updates the running desktop.");
    gtk_label_set_line_wrap(GTK_LABEL(status_), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(status_), 65);
    gtk_label_set_xalign(GTK_LABEL(status_), 0);
    gtk_box_pack_start(GTK_BOX(outer), status_, FALSE, FALSE, 0);
    auto* buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(buttons), 8);
    gtk_box_pack_start(GTK_BOX(outer), buttons, FALSE, FALSE, 0);
    auto* close = gtk_button_new_with_mnemonic("_Close");
    gtk_container_add(GTK_CONTAINER(buttons), close);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), window_);
    auto* apply = gtk_button_new_with_mnemonic("_Apply");
    gtk_container_add(GTK_CONTAINER(buttons), apply);
    g_signal_connect(apply, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      static_cast<Settings*>(data)->apply();
    }), this);
    if (!valid_) {
      gtk_widget_set_sensitive(notebook, FALSE);
      gtk_widget_set_sensitive(apply, FALSE);
      gtk_label_set_text(GTK_LABEL(status_), ("Cannot read settings: " + error + "\nFix the file and reopen Settings; it has not been changed.").c_str());
    }
    gtk_widget_show_all(window_);
  }

 private:
  std::filesystem::path path_;
  KeyFile file_;
  std::string original_;
  bool existed_{}, valid_{};
  GtkWidget* window_{};
  GtkWidget* status_{};
  int row_{};
  std::vector<std::unique_ptr<Binding>> bindings_;
  struct DefaultBinding {
    midnight::AppDefaultRole role;
    GtkWidget* combo{};
    std::string initial;
    bool changed{};
  };
  std::vector<std::unique_ptr<DefaultBinding>> default_bindings_;
  GtkWidget* startup_list_{};
  GtkWidget* startup_name_{};
  GtkWidget* startup_command_{};
  std::array<GtkWidget*, static_cast<std::size_t>(midnight::ShortcutAction::count)> shortcut_entries_{};
  std::array<bool, static_cast<std::size_t>(midnight::ShortcutAction::count)> shortcut_changed_{};

  void shortcut_page(GtkWidget* grid) {
    auto* help = gtk_label_new("Type a shortcut such as Ctrl+Alt+Left or Super+D. Use None to disable it. Changes apply to the running desktop after Apply.");
    gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
    gtk_label_set_xalign(GTK_LABEL(help), 0);
    gtk_grid_attach(GTK_GRID(grid), help, 0, row_++, 2, 1);
    const auto defaults = midnight::default_shortcuts();
    for (std::size_t i = 0; i < shortcut_entries_.size(); ++i) {
      const auto action = static_cast<midnight::ShortcutAction>(i);
      auto* widget = gtk_entry_new();
      char* configured = g_key_file_get_string(file_.get(), "Shortcuts", midnight::shortcut_key(action), nullptr);
      const auto value = configured ? std::string(configured) : midnight::format_shortcut(defaults[i]);
      g_free(configured);
      gtk_entry_set_text(GTK_ENTRY(widget), value.c_str());
      gtk_widget_set_tooltip_text(widget, "Ctrl, Alt, Super and Shift modifiers; letters, digits, F1-F35, arrows, Tab, and common navigation keys. None disables.");
      add(grid, midnight::shortcut_label(action), widget);
      shortcut_entries_[i] = widget;
      g_signal_connect(widget, "changed", G_CALLBACK(+[](GtkEditable* editable, gpointer data) {
        auto* self = static_cast<Settings*>(data);
        for (std::size_t index = 0; index < self->shortcut_entries_.size(); ++index)
          if (self->shortcut_entries_[index] == GTK_WIDGET(editable)) self->shortcut_changed_[index] = true;
      }), this);
    }
    auto* reset = gtk_button_new_with_label("Reset shortcuts to defaults");
    gtk_grid_attach(GTK_GRID(grid), reset, 0, row_++, 2, 1);
    g_signal_connect(reset, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      auto* self = static_cast<Settings*>(data);
      const auto defaults = midnight::default_shortcuts();
      for (std::size_t i = 0; i < self->shortcut_entries_.size(); ++i) {
        gtk_entry_set_text(GTK_ENTRY(self->shortcut_entries_[i]), midnight::format_shortcut(defaults[i]).c_str());
        self->shortcut_changed_[i] = true;
      }
    }), this);
  }

  std::expected<midnight::Shortcuts, std::string> edited_shortcuts() const {
    midnight::Shortcuts shortcuts{};
    for (std::size_t i = 0; i < shortcuts.size(); ++i) {
      const auto action = static_cast<midnight::ShortcutAction>(i);
      const auto parsed = midnight::parse_shortcut(gtk_entry_get_text(GTK_ENTRY(shortcut_entries_[i])));
      if (!parsed) return std::unexpected(std::string(midnight::shortcut_label(action)) + ": " + parsed.error());
      shortcuts[i] = *parsed;
    }
    for (std::size_t i = 0; i < shortcuts.size(); ++i)
      for (std::size_t j = i + 1; j < shortcuts.size(); ++j)
        if (shortcuts[i] && shortcuts[i] == shortcuts[j])
          return std::unexpected(std::string(midnight::shortcut_label(static_cast<midnight::ShortcutAction>(i))) +
              " conflicts with " + midnight::shortcut_label(static_cast<midnight::ShortcutAction>(j)));
    return shortcuts;
  }

  static std::vector<std::filesystem::path> startup_roots() {
    std::vector<std::filesystem::path> roots{g_get_user_config_dir()};
    for (auto dirs = g_get_system_config_dirs(); dirs && *dirs; ++dirs) roots.emplace_back(*dirs);
    return roots;
  }

  void refresh_startup_applications() {
    auto* children = gtk_container_get_children(GTK_CONTAINER(startup_list_));
    for (auto* item = children; item; item = item->next) gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);
    const auto applications = midnight::startup_applications(startup_roots());
    if (applications.empty()) {
      auto* empty = gtk_label_new("No startup applications were found.");
      gtk_label_set_xalign(GTK_LABEL(empty), 0);
      gtk_box_pack_start(GTK_BOX(startup_list_), empty, FALSE, FALSE, 0);
    }
    for (const auto& application : applications) {
      auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      auto* check = gtk_check_button_new();
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), application.enabled);
      gtk_widget_set_sensitive(check, application.available);
      gtk_widget_set_tooltip_text(check, application.available ?
          "Start this application at the next login" : "This entry does not apply to Midnight Workspace");
      auto* details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      auto* name = gtk_label_new(application.name.c_str());
      gtk_label_set_xalign(GTK_LABEL(name), 0);
      auto* command = gtk_label_new((application.command + "  ·  " +
          (application.user_entry ? "User" : "System")).c_str());
      gtk_label_set_xalign(GTK_LABEL(command), 0);
      gtk_label_set_ellipsize(GTK_LABEL(command), PANGO_ELLIPSIZE_END);
      gtk_widget_set_tooltip_text(command, application.path.c_str());
      gtk_box_pack_start(GTK_BOX(details), name, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(details), command, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(row), check, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(row), details, TRUE, TRUE, 0);
      auto* copy = new midnight::StartupApplication(application);
      g_object_set_data_full(G_OBJECT(check), "startup-application", copy,
          +[](gpointer data) { delete static_cast<midnight::StartupApplication*>(data); });
      g_signal_connect(check, "toggled", G_CALLBACK(+[](GtkToggleButton* toggle, gpointer data) {
        auto* self = static_cast<Settings*>(data);
        const auto* selected = static_cast<midnight::StartupApplication*>(
            g_object_get_data(G_OBJECT(toggle), "startup-application"));
        const auto result = midnight::set_startup_application_enabled(*selected,
            gtk_toggle_button_get_active(toggle), g_get_user_config_dir());
        if (!result) self->fail(result.error());
        else gtk_label_set_text(GTK_LABEL(self->status_), "Startup change saved; it takes effect at the next login.");
        self->refresh_startup_applications();
      }), this);
      if (application.user_entry) {
        auto* remove = gtk_button_new_with_label("Remove…");
        auto* remove_copy = new midnight::StartupApplication(application);
        g_object_set_data_full(G_OBJECT(remove), "startup-application", remove_copy,
            +[](gpointer data) { delete static_cast<midnight::StartupApplication*>(data); });
        g_signal_connect(remove, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
          auto* self = static_cast<Settings*>(data);
          const auto* selected = static_cast<midnight::StartupApplication*>(
              g_object_get_data(G_OBJECT(button), "startup-application"));
          auto* dialog = gtk_message_dialog_new(GTK_WINDOW(self->window_), GTK_DIALOG_MODAL,
              GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
              "Remove the user startup entry for %s?", selected->name.c_str());
          gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
              "This deletes %s. If a system entry has the same filename, it will become active again.",
              selected->path.c_str());
          gtk_dialog_add_buttons(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL,
              "_Remove", GTK_RESPONSE_ACCEPT, nullptr);
          const bool confirmed = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
          gtk_widget_destroy(dialog);
          if (!confirmed) return;
          const auto result = midnight::remove_startup_application(*selected, g_get_user_config_dir());
          if (!result) self->fail(result.error());
          else gtk_label_set_text(GTK_LABEL(self->status_), "Startup entry removed; the change takes effect at the next login.");
          self->refresh_startup_applications();
        }), this);
        gtk_box_pack_start(GTK_BOX(row), remove, FALSE, FALSE, 0);
      }
      gtk_box_pack_start(GTK_BOX(startup_list_), row, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(startup_list_);
  }

  void startup_applications_page(GtkWidget* grid) {
    auto* help = gtk_label_new("Choose what starts at the next login. Changes on this page save immediately. System entries are disabled through a user override.");
    gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
    gtk_label_set_xalign(GTK_LABEL(help), 0);
    gtk_grid_attach(GTK_GRID(grid), help, 0, row_++, 2, 1);
    startup_list_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_hexpand(startup_list_, TRUE);
    gtk_grid_attach(GTK_GRID(grid), startup_list_, 0, row_++, 2, 1);
    startup_name_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(startup_name_), "Application name");
    add(grid, "_Name", startup_name_);
    startup_command_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(startup_command_), "Desktop entry command, e.g. /usr/local/bin/example");
    add(grid, "_Command", startup_command_);
    auto* add_button = gtk_button_new_with_mnemonic("C_reate startup application");
    gtk_grid_attach(GTK_GRID(grid), add_button, 0, row_++, 2, 1);
    g_signal_connect(add_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      auto* self = static_cast<Settings*>(data);
      const auto result = midnight::add_startup_application(
          gtk_entry_get_text(GTK_ENTRY(self->startup_name_)),
          gtk_entry_get_text(GTK_ENTRY(self->startup_command_)), g_get_user_config_dir());
      if (!result) { self->fail(result.error()); return; }
      gtk_entry_set_text(GTK_ENTRY(self->startup_name_), "");
      gtk_entry_set_text(GTK_ENTRY(self->startup_command_), "");
      gtk_label_set_text(GTK_LABEL(self->status_), "Startup application added; it starts at the next login.");
      self->refresh_startup_applications();
    }), this);
    refresh_startup_applications();
  }

  bool read_file(std::string& text, bool& exists, std::string& message) const {
    gchar* contents = nullptr;
    gsize size = 0;
    GError* error = nullptr;
    if (!g_file_get_contents(path_.c_str(), &contents, &size, &error)) {
      exists = false;
      if (g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) { g_error_free(error); text.clear(); return true; }
      message = error->message;
      g_error_free(error);
      return false;
    }
    exists = true;
    text.assign(contents, size);
    g_free(contents);
    return true;
  }
  std::string string(const char* group, const char* key, const char* fallback) const {
    gchar* value = g_key_file_get_string(file_.get(), group, key, nullptr);
    std::string result = value ? value : fallback;
    g_free(value);
    return result;
  }
  bool boolean(const char* group, const char* key, bool fallback) const {
    GError* error = nullptr;
    const bool value = g_key_file_get_boolean(file_.get(), group, key, &error);
    if (error) { g_error_free(error); return fallback; }
    return value;
  }
  Binding* bind(GtkWidget* widget, const char* signal, std::function<void(GKeyFile*)> save) {
    auto binding = std::make_unique<Binding>();
    binding->save = std::move(save);
    auto* result = binding.get();
    g_signal_connect(widget, signal, G_CALLBACK(+[](GtkWidget*, gpointer data) {
      static_cast<Binding*>(data)->changed = true;
    }), result);
    bindings_.push_back(std::move(binding));
    return result;
  }
  GtkWidget* page(GtkWidget* notebook, const char* title) {
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    auto* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
    gtk_container_add(GTK_CONTAINER(scroll), grid);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll, gtk_label_new(title));
    return grid;
  }
  void add(GtkWidget* grid, const char* title, GtkWidget* widget) {
    auto* label = gtk_label_new_with_mnemonic(title);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), widget);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row_, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), widget, 1, row_++, 1, 1);
  }
  void application_defaults(GtkWidget* grid) {
    auto* help = gtk_label_new("Choose applications for web links, HTML files, and other content, then click Apply. The terminal choice also controls the panel button.");
    gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
    gtk_label_set_xalign(GTK_LABEL(help), 0);
    gtk_grid_attach(GTK_GRID(grid), help, 0, row_++, 2, 1);
    for (const auto role : {midnight::AppDefaultRole::browser, midnight::AppDefaultRole::html,
             midnight::AppDefaultRole::mail,
             midnight::AppDefaultRole::calendar, midnight::AppDefaultRole::terminal,
             midnight::AppDefaultRole::files, midnight::AppDefaultRole::text,
             midnight::AppDefaultRole::images, midnight::AppDefaultRole::pdf}) {
      auto* combo = gtk_combo_box_text_new();
      const auto current = midnight::current_app_default(role);
      if (!current) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "", "Choose an application…");
      const auto choices = midnight::app_default_choices(role);
      for (const auto& choice : choices)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), choice.id.c_str(),
            (choice.name + " (" + choice.id + ")").c_str());
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), current ? current->id.c_str() : "");
      add(grid, midnight::app_default_label(role), combo);
      auto binding = std::make_unique<DefaultBinding>();
      binding->role = role;
      binding->combo = combo;
      binding->initial = current ? current->id : "";
      g_signal_connect(combo, "changed", G_CALLBACK(+[](GtkComboBox* selected, gpointer data) {
        auto& binding = *static_cast<DefaultBinding*>(data);
        const char* id = gtk_combo_box_get_active_id(selected);
        binding.changed = id && *id && id != binding.initial;
      }), binding.get());
      default_bindings_.push_back(std::move(binding));
    }
  }
  void checkbox(GtkWidget* grid, const char* title, const char* group, const char* key, bool fallback) {
    auto* check = gtk_check_button_new_with_mnemonic(title);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), boolean(group, key, fallback));
    gtk_grid_attach(GTK_GRID(grid), check, 0, row_++, 2, 1);
    bind(check, "toggled", [check, group, key](GKeyFile* file) {
      g_key_file_set_boolean(file, group, key, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)));
    });
  }
  void entry(GtkWidget* grid, const char* title, const char* key, const char* fallback) {
    auto* widget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(widget), string("Workspace", key, fallback).c_str());
    add(grid, title, widget);
    bind(widget, "changed", [widget, key](GKeyFile* file) {
      g_key_file_set_string(file, "Workspace", key, gtk_entry_get_text(GTK_ENTRY(widget)));
    });
  }
  static void save_color(GKeyFile* file, const char* key, GtkWidget* button) {
    GdkRGBA rgba{};
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);
    auto* value = g_strdup_printf("#%02x%02x%02x", static_cast<unsigned>(std::lround(rgba.red * 255)),
        static_cast<unsigned>(std::lround(rgba.green * 255)), static_cast<unsigned>(std::lround(rgba.blue * 255)));
    g_key_file_set_string(file, "Workspace", key, value);
    g_free(value);
  }
  GtkWidget* color_button(const char* key, const char* fallback) {
    GdkRGBA rgba{};
    if (!gdk_rgba_parse(&rgba, string("Workspace", key, fallback).c_str())) gdk_rgba_parse(&rgba, fallback);
    return gtk_color_button_new_with_rgba(&rgba);
  }
  void color(GtkWidget* grid, const char* title, const char* key, const char* fallback) {
    auto* button = color_button(key, fallback);
    add(grid, title, button);
    bind(button, "color-set", [button, key](GKeyFile* file) { save_color(file, key, button); });
  }
  void panel_color(GtkWidget* grid) {
    auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* enabled = gtk_check_button_new_with_label("Custom");
    auto* button = color_button("panel_color", "#c4c4c4");
    const bool custom = !string("Workspace", "panel_color", "").empty();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled), custom);
    gtk_widget_set_sensitive(button, custom);
    gtk_box_pack_start(GTK_BOX(box), enabled, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), button, TRUE, TRUE, 0);
    add(grid, "Panel color", box);
    auto save = [enabled, button](GKeyFile* file) {
      if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enabled))) save_color(file, "panel_color", button);
      else g_key_file_remove_key(file, "Workspace", "panel_color", nullptr);
    };
    bind(enabled, "toggled", save);
    bind(button, "color-set", save);
    g_signal_connect(enabled, "toggled", G_CALLBACK(+[](GtkToggleButton* toggle, gpointer data) {
      gtk_widget_set_sensitive(GTK_WIDGET(data), gtk_toggle_button_get_active(toggle));
    }), button);
  }
  void font(GtkWidget* grid, const char* title, const char* key, const char* fallback) {
    auto* description = pango_font_description_from_string(string("Workspace", key, fallback).c_str());
    const bool default_sizes = !(pango_font_description_get_set_fields(description) & PANGO_FONT_MASK_SIZE);
    if (default_sizes) pango_font_description_set_size(description, 10 * PANGO_SCALE);
    auto* button = gtk_font_button_new();
    gtk_font_chooser_set_font_desc(GTK_FONT_CHOOSER(button), description);
    pango_font_description_free(description);
    gtk_font_button_set_show_size(GTK_FONT_BUTTON(button), !default_sizes);
    if (default_sizes)
      gtk_widget_set_tooltip_text(button, "Default widget sizes are retained until you choose a font. The chooser starts at 10 points.");
    gtk_font_button_set_use_font(GTK_FONT_BUTTON(button), TRUE);
    add(grid, title, button);
    g_signal_connect(button, "font-set", G_CALLBACK(+[](GtkFontButton* selected, gpointer) {
      gtk_font_button_set_show_size(selected, TRUE);
      gtk_widget_set_tooltip_text(GTK_WIDGET(selected), "Font size is saved in points, from 6 to 32.");
    }), nullptr);
    bind(button, "font-set", [button, key](GKeyFile* file) {
      auto* description = gtk_font_chooser_get_font_desc(GTK_FONT_CHOOSER(button));
      if (!description) return;
      if (pango_font_description_get_set_fields(description) & PANGO_FONT_MASK_SIZE) {
        const int size = std::clamp(pango_font_description_get_size(description), 6 * PANGO_SCALE, 32 * PANGO_SCALE);
        pango_font_description_set_size(description, size);
      }
      auto* value = pango_font_description_to_string(description);
      g_key_file_set_string(file, "Workspace", key, value);
      g_free(value);
      pango_font_description_free(description);
    });
  }
  void scaling(GtkWidget* grid) {
    auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* automatic = gtk_check_button_new_with_mnemonic("A_utomatic");
    auto* value = gtk_spin_button_new_with_range(.75, 3, .05);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(value), 2);
    const auto text = string("Workspace", "ui_scale", "");
    char* end = nullptr;
    const double scale = g_ascii_strtod(text.c_str(), &end);
    const bool custom = end && *end == '\0' && std::isfinite(scale) && scale >= .75 && scale <= 3;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(automatic), !custom);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(value), custom ? scale : 1);
    gtk_widget_set_sensitive(value, custom);
    gtk_box_pack_start(GTK_BOX(box), automatic, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), value, TRUE, TRUE, 0);
    add(grid, "Interface scale", box);
    auto save = [automatic, value](GKeyFile* file) {
      if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(automatic))) g_key_file_remove_key(file, "Workspace", "ui_scale", nullptr);
      else g_key_file_set_double(file, "Workspace", "ui_scale", gtk_spin_button_get_value(GTK_SPIN_BUTTON(value)));
    };
    bind(automatic, "toggled", save);
    bind(value, "value-changed", save);
    g_signal_connect(automatic, "toggled", G_CALLBACK(+[](GtkToggleButton* toggle, gpointer data) {
      gtk_widget_set_sensitive(GTK_WIDGET(data), !gtk_toggle_button_get_active(toggle));
    }), value);
  }
  static gboolean fit_wallpaper_chooser(gpointer data) {
    auto* widget = GTK_WIDGET(data);
    if (!gtk_widget_get_visible(widget)) return G_SOURCE_REMOVE;
    auto* parent = gtk_window_get_transient_for(GTK_WINDOW(widget));
    auto* parent_window = parent ? gtk_widget_get_window(GTK_WIDGET(parent)) : nullptr;
    auto* display = gtk_widget_get_display(widget);
    auto* monitor = parent_window ? gdk_display_get_monitor_at_window(display, parent_window)
                                  : gdk_display_get_primary_monitor(display);
    if (monitor) {
      GdkRectangle area{};
      gdk_monitor_get_workarea(monitor, &area);
      gtk_window_resize(GTK_WINDOW(widget), std::min(760, std::max(1, area.width - 40)),
                        std::min(480, std::max(1, area.height - 80)));
    }
    return G_SOURCE_REMOVE;
  }
  void wallpaper(GtkWidget* grid) {
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    auto* value = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(value), string("Workspace", "desktop_image", midnight::default_desktop_image).c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(value), "No image (desktop color)");
    gtk_widget_set_tooltip_text(value, "Full path or installed wallpaper filename. PNG, JPEG, GIF (first frame), SVG.");
    gtk_box_pack_start(GTK_BOX(box), value, FALSE, FALSE, 0);
    auto* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* browse = gtk_button_new_with_mnemonic("Choose _image…");
    auto* clear = gtk_button_new_with_label("Clear");
    gtk_box_pack_start(GTK_BOX(buttons), browse, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    add(grid, "Wallpaper", box);
    bind(value, "changed", [value](GKeyFile* file) {
      g_key_file_set_string(file, "Workspace", "desktop_image", gtk_entry_get_text(GTK_ENTRY(value)));
    });
    g_signal_connect(clear, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      gtk_entry_set_text(GTK_ENTRY(data), "");
    }), value);
    g_signal_connect(browse, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
      auto* dialog = gtk_file_chooser_dialog_new("Choose desktop background", GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
          GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, nullptr);
      // The chooser restores its own preferred size while mapping, overriding
      // an earlier default-size request. Resize after that initialization.
      g_signal_connect(dialog, "map", G_CALLBACK(+[](GtkWidget* mapped, gpointer) {
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, fit_wallpaper_chooser,
                        g_object_ref(mapped), g_object_unref);
      }), nullptr);
      gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
      const char* current = gtk_entry_get_text(GTK_ENTRY(data));
      if (g_path_is_absolute(current) && g_file_test(current, G_FILE_TEST_IS_REGULAR))
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), current);
      else gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), MIDNIGHT_WM_BACKGROUND_DIR);
      auto* filter = gtk_file_filter_new();
      gtk_file_filter_set_name(filter, "Desktop images (PNG, JPEG, GIF, SVG)");
      for (const char* type : {"image/png", "image/jpeg", "image/gif", "image/svg+xml"}) gtk_file_filter_add_mime_type(filter, type);
      gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
      auto* preview = gtk_image_new();
      gtk_file_chooser_set_preview_widget(GTK_FILE_CHOOSER(dialog), preview);
      g_signal_connect(dialog, "update-preview", G_CALLBACK(+[](GtkFileChooser* chooser, gpointer image) {
        auto* filename = gtk_file_chooser_get_preview_filename(chooser);
        auto* pixbuf = filename ? gdk_pixbuf_new_from_file_at_scale(filename, 240, 160, TRUE, nullptr) : nullptr;
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
        gtk_file_chooser_set_preview_widget_active(chooser, pixbuf != nullptr);
        if (pixbuf) g_object_unref(pixbuf);
        g_free(filename);
      }), preview);
      if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        auto* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) gtk_entry_set_text(GTK_ENTRY(data), filename);
        g_free(filename);
      }
      gtk_widget_destroy(dialog);
    }), value);
  }
  void apply() {
    if (!valid_) return;
    const bool shortcuts_changed = std::ranges::any_of(shortcut_changed_, [](bool changed) { return changed; });
    const bool ini_changed = shortcuts_changed ||
        std::ranges::any_of(bindings_, [](const auto& binding) { return binding->changed; });
    const bool defaults_changed = std::ranges::any_of(default_bindings_, [](const auto& binding) { return binding->changed; });
    if (!ini_changed && !defaults_changed) {
      gtk_label_set_text(GTK_LABEL(status_), "No changes to apply.");
      return;
    }
    if (ini_changed) {
      std::optional<midnight::Shortcuts> shortcuts;
      if (shortcuts_changed) {
        const auto edited = edited_shortcuts();
        if (!edited) { fail(edited.error()); return; }
        shortcuts = *edited;
      }
      std::string current, error;
      bool exists = false;
      if (!read_file(current, exists, error)) { fail(error); return; }
      if (exists != existed_ || current != original_) {
        fail("The settings file changed outside this window. Close and reopen Settings before applying changes.");
        return;
      }
      KeyFile updated(g_key_file_new());
      if (existed_) g_key_file_load_from_data(updated.get(), original_.data(), original_.size(),
          static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS), nullptr);
      for (auto& binding : bindings_) if (binding->changed) binding->save(updated.get());
      if (shortcuts) {
        const auto builtins = midnight::default_shortcuts();
        for (std::size_t i = 0; i < shortcuts->size(); ++i) {
          if (!shortcut_changed_[i]) continue;
          const auto action = static_cast<midnight::ShortcutAction>(i);
          if ((*shortcuts)[i] == builtins[i])
            g_key_file_remove_key(updated.get(), "Shortcuts", midnight::shortcut_key(action), nullptr);
          else g_key_file_set_string(updated.get(), "Shortcuts", midnight::shortcut_key(action),
              midnight::format_shortcut((*shortcuts)[i]).c_str());
        }
      }
      gsize length = 0;
      auto* data = g_key_file_to_data(updated.get(), &length, nullptr);
      if (g_mkdir_with_parents(path_.parent_path().c_str(), 0700) != 0) {
        g_free(data);
        fail("Could not create the settings directory.");
        return;
      }
      GError* write_error = nullptr;
      const bool saved = g_file_set_contents_full(path_.c_str(), data, static_cast<gssize>(length),
          static_cast<GFileSetContentsFlags>(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE), 0600, &write_error);
      if (!saved) {
        fail(write_error ? write_error->message : "Could not save settings");
        g_clear_error(&write_error);
        g_free(data);
        return;
      }
      original_.assign(data, length);
      existed_ = true;
      file_ = std::move(updated);
      for (auto& binding : bindings_) binding->changed = false;
      shortcut_changed_.fill(false);
      g_free(data);
    }
    if (defaults_changed) {
      std::vector<midnight::AppDefaultSetting> changes;
      for (const auto& binding : default_bindings_) if (binding->changed) {
        const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(binding->combo));
        if (id && *id) changes.push_back({binding->role, id});
      }
      const auto result = midnight::save_app_defaults(changes);
      if (!result) {
        fail((ini_changed ? "Workspace settings saved, but application defaults failed: " :
            "Could not save application defaults: ") + result.error());
        return;
      }
      for (auto& binding : default_bindings_) if (binding->changed) {
        binding->initial = gtk_combo_box_get_active_id(GTK_COMBO_BOX(binding->combo));
        binding->changed = false;
      }
    }
    gtk_label_set_text(GTK_LABEL(status_), ini_changed ?
        "Settings applied. The running Midnight Workspace updates automatically." :
        "Application defaults saved.");
  }
  void fail(const std::string& message) {
    auto* dialog = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
        GTK_BUTTONS_CLOSE, "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }
};
} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--version") {
    g_print("midnightbsd-settings %s\n", MIDNIGHT_WM_VERSION);
    return 0;
  }
  gtk_init(&argc, &argv);
  Settings settings;
  gtk_main();
}
