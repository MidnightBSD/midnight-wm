// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include <gtk/gtk.h>
#include <cstring>
#include <string>
#include "display_text.hpp"

namespace {
struct Search {
  GtkWidget *window{}, *entry{}, *find{}, *list{}, *status{};
  GSubprocess* process{};
  guint timeout{};

  void message(const char* text) { gtk_label_set_text(GTK_LABEL(status), text); }
  void start() {
    const char* term = gtk_entry_get_text(GTK_ENTRY(entry));
    if (process || !*term) return;
    auto* children = gtk_container_get_children(GTK_CONTAINER(list));
    for (auto* p = children; p; p = p->next) gtk_widget_destroy(GTK_WIDGET(p->data));
    g_list_free(children);
    // Use literal substring matching even when the term contains locate glob syntax.
    std::string pattern = "*";
    for (const char* p = term; *p; ++p) {
      if (std::strchr("*?[]\\", *p)) pattern += '\\';
      pattern += *p;
    }
    pattern += '*';
    GError* error = nullptr;
    process = g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_SILENCE), &error, "/usr/bin/locate", "-0", "-i", "-l", "20",
        "--", pattern.c_str(), nullptr);
    if (!process) {
      message(error ? error->message : "Could not start locate.");
      g_clear_error(&error);
      return;
    }
    gtk_widget_set_sensitive(find, FALSE);
    gtk_widget_set_sensitive(entry, FALSE);
    message("Searching…");
    timeout = g_timeout_add_seconds(15, +[](gpointer data) -> gboolean {
      auto& self = *static_cast<Search*>(data);
      self.timeout = 0;
      if (self.process) g_subprocess_force_exit(self.process);
      return G_SOURCE_REMOVE;
    }, this);
    g_subprocess_communicate_async(process, nullptr, nullptr,
        +[](GObject* object, GAsyncResult* result, gpointer data) {
      auto& self = *static_cast<Search*>(data);
      if (self.timeout) { g_source_remove(self.timeout); self.timeout = 0; }
      GBytes* output = nullptr;
      GError* failure = nullptr;
      const bool ok = g_subprocess_communicate_finish(G_SUBPROCESS(object), result, &output, nullptr, &failure);
      if (!self.window) {
        if (output) g_bytes_unref(output);
        g_clear_error(&failure);
        g_clear_object(&self.process);
        return;
      }
      unsigned count = 0;
      if (ok && g_subprocess_get_successful(self.process)) {
        gsize size = 0;
        const auto* bytes = static_cast<const char*>(g_bytes_get_data(output, &size));
        for (gsize pos = 0; pos < size && count < 20;) {
          const auto* end = static_cast<const char*>(std::memchr(bytes + pos, 0, size - pos));
          if (!end) break;
          std::string path(bytes + pos, end);
          pos += path.size() + 1;
          auto* row = gtk_list_box_row_new();
          // Strip controls and bidi formatting so another user's file name cannot
          // disguise which file a click opens.
          auto* display = g_filename_display_name(path.c_str());
          const std::string shown = midnight::sanitize_display_text(display);
          g_free(display);
          auto* label = gtk_label_new(shown.c_str());
          gtk_label_set_xalign(GTK_LABEL(label), 0);
          gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
          gtk_widget_set_tooltip_text(row, gtk_label_get_text(GTK_LABEL(label)));
          gtk_widget_set_margin_top(label, 5);
          gtk_widget_set_margin_bottom(label, 5);
          gtk_container_add(GTK_CONTAINER(row), label);
          g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path.c_str()), g_free);
          gtk_container_add(GTK_CONTAINER(self.list), row);
          ++count;
        }
        self.message(count ? "Up to 20 matches from the locate database. Click a file to open it." : "No matches in the locate database.");
      } else self.message("Search failed or timed out. The locate database may be unavailable.");
      if (output) g_bytes_unref(output);
      g_clear_error(&failure);
      g_clear_object(&self.process);
      gtk_widget_show_all(self.list);
      gtk_widget_set_sensitive(self.find, TRUE);
      gtk_widget_set_sensitive(self.entry, TRUE);
      gtk_widget_grab_focus(self.entry);
    }, this);
  }
};
}

int main(int argc, char** argv) {
  auto* app = gtk_application_new("org.midnightbsd.Workspace.Search", G_APPLICATION_DEFAULT_FLAGS);
  Search search;
  g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication* application, gpointer data) {
    auto& s = *static_cast<Search*>(data);
    if (s.window) { gtk_window_present(GTK_WINDOW(s.window)); return; }
    s.window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(s.window), "Search Files");
    gtk_window_set_icon_name(GTK_WINDOW(s.window), "system-search");
    gtk_window_set_default_size(GTK_WINDOW(s.window), 620, 490);
    gtk_window_set_position(GTK_WINDOW(s.window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(s.window), 12);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(s.window), box);
    auto* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    s.entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(s.entry), "File name or path");
    gtk_entry_set_max_length(GTK_ENTRY(s.entry), 1024);
    gtk_widget_set_tooltip_text(s.entry, "Search the locate database (case insensitive)");
    gtk_box_pack_start(GTK_BOX(bar), s.entry, TRUE, TRUE, 0);
    s.find = gtk_button_new_with_mnemonic("_Find");
    gtk_box_pack_start(GTK_BOX(bar), s.find, FALSE, FALSE, 0);
    for (auto* widget : {s.entry, s.find})
      g_signal_connect(widget, widget == s.entry ? "activate" : "clicked",
          G_CALLBACK(+[](GtkWidget*, gpointer value) { static_cast<Search*>(value)->start(); }), &s);
    s.status = gtk_label_new("Search files indexed by the system locate database.");
    gtk_label_set_line_wrap(GTK_LABEL(s.status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(s.status), 0);
    gtk_box_pack_start(GTK_BOX(box), s.status, FALSE, FALSE, 0);
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    s.list = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(s.list), TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), s.list);
    g_signal_connect(s.list, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer value) {
      auto& self = *static_cast<Search*>(value);
      auto* path = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "path"));
      auto* file = g_file_new_for_path(path);
      auto* uri = g_file_get_uri(file);
      auto* context = gdk_display_get_app_launch_context(gtk_widget_get_display(self.window));
      GError* error = nullptr;
      if (!g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(context), &error)) {
        self.message(error ? error->message : "Could not open file.");
        g_clear_error(&error);
      }
      g_object_unref(context);
      g_free(uri);
      g_object_unref(file);
    }), &s);
    auto* close = gtk_button_new_with_mnemonic("_Close");
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(box), close, FALSE, FALSE, 0);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_close), s.window);
    g_signal_connect(s.window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer value) {
      auto& self = *static_cast<Search*>(value);
      // Keep callbacks from accessing destroyed controls while the application quits.
      if (self.process) g_subprocess_force_exit(self.process);
      self.window = nullptr;
    }), &s);
    gtk_widget_show_all(s.window);
    gtk_widget_grab_focus(s.entry);
  }), &search);
  const int result = g_application_run(G_APPLICATION(app), argc, argv);
  if (search.timeout) g_source_remove(search.timeout);
  if (search.process) g_object_unref(search.process);
  g_object_unref(app);
  return result;
}
