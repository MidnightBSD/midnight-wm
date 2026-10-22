// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "chat_core.hpp"
#include "chat_tools.hpp"
#include <gtk/gtk.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace {
using namespace midnight::chat;
std::string contents(GtkWidget* view) {
  auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  GtkTextIter first, last; gtk_text_buffer_get_bounds(buffer, &first, &last);
  auto* text = gtk_text_buffer_get_text(buffer, &first, &last, FALSE);
  std::string result(text); g_free(text); return result;
}
void accessible(GtkWidget* w, const char* name) { atk_object_set_name(gtk_widget_get_accessible(w), name); }
struct Job {
  Settings settings;
  Thread thread;
  bool listing{};
  std::vector<std::string> models;
  std::string error;
};
struct Chat {
  GtkApplication* app{};
  GtkWidget *window{}, *list{}, *transcript{}, *input{}, *status{}, *send{}, *stop{}, *model{}, *refresh{}, *settings_button{}, *remove{}, *reset{};
  std::filesystem::path settings_path{std::filesystem::path(g_get_user_config_dir()) / "midnightbsd-chat/settings.json"};
  std::filesystem::path history_path{std::filesystem::path(g_get_user_data_dir()) / "midnightbsd-chat/conversations.json"};
  Settings settings;
  std::vector<Thread> threads;
  std::string selected, active;
  std::unordered_map<std::string, std::string> drafts;
  guint deadline{};
  bool timed_out{};
  GCancellable* cancellable{};
  bool history_ok{true}, settings_ok{true}, rebuilding{};
  ~Chat() { if (cancellable) g_object_unref(cancellable); }
  Thread* current() {
    auto it = std::find_if(threads.begin(), threads.end(), [&](const auto& t) { return t.id == selected; });
    return it == threads.end() ? nullptr : &*it;
  }
  void message(const std::string& text) { if (window) gtk_label_set_text(GTK_LABEL(status), text.c_str()); }
  void error_dialog(const std::string& text) {
    auto* d = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", text.c_str());
    gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
  }
  bool persist() {
    if (!history_ok) return false;
    try { save_threads(history_path, threads); return true; }
    catch (const std::exception& e) { message(std::string("Could not save conversations: ") + e.what()); return false; }
  }
  void sensitivity() {
    if (!window) return;
    const bool busy = cancellable != nullptr;
    gtk_widget_set_sensitive(send, history_ok && !busy && current());
    gtk_widget_set_sensitive(stop, busy);
    gtk_widget_set_sensitive(refresh, settings_ok && !busy);
    gtk_widget_set_sensitive(settings_button, settings_ok && !busy);
    gtk_widget_set_sensitive(model, settings_ok && !busy);
    gtk_widget_set_sensitive(remove, history_ok && current() && selected != active);
    gtk_widget_set_sensitive(reset, history_ok && current() && selected != active);
  }
  void render() {
    auto* t = current(); std::string text;
    if (t) {
      if (t->compacted) text = "[Earlier context summarized or reset; full transcript retained below.]\n\n";
      for (const auto& m : t->messages) {
        for (const auto& tool : m.tools)
          text += "Tool: " + tool.name + "\nArguments: " + tool.arguments + "\n" + tool.result + "\n\n";
        text += (m.role == "user" ? "You\n" : "Assistant\n") + m.text + "\n\n";
      }
      if (t->messages.empty()) text = "Start a conversation below. Choose your server and model in Settings.\n";
    }
    auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(transcript));
    gtk_text_buffer_set_text(buffer, text.c_str(), text.size());
    GtkTextIter end; gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_place_cursor(buffer, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(transcript), gtk_text_buffer_get_insert(buffer));
    sensitivity();
  }
  void rebuild() {
    rebuilding = true;
    auto* children = gtk_container_get_children(GTK_CONTAINER(list));
    for (auto* p = children; p; p = p->next) gtk_widget_destroy(GTK_WIDGET(p->data));
    g_list_free(children);
    for (const auto& t : threads) {
      auto* label = gtk_label_new(t.title.c_str());
      gtk_label_set_xalign(GTK_LABEL(label), 0);
      gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_margin_start(label, 8); gtk_widget_set_margin_end(label, 8);
      gtk_widget_set_margin_top(label, 10); gtk_widget_set_margin_bottom(label, 10);
      auto* row = gtk_list_box_row_new(); gtk_container_add(GTK_CONTAINER(row), label);
      g_object_set_data_full(G_OBJECT(row), "thread", g_strdup(t.id.c_str()), g_free);
      gtk_container_add(GTK_CONTAINER(list), row);
      if (t.id == selected) gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(row));
    }
    gtk_widget_show_all(list); rebuilding = false; render();
  }
  void select(const std::string& id) {
    if (!selected.empty()) drafts[selected] = contents(input);
    selected = id;
    const auto& text = drafts[id];
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(input)), text.c_str(), text.size());
  }
  void new_thread() {
    if (!history_ok) return;
    char* id = g_uuid_string_random(); Thread t; t.id = id; g_free(id);
    auto previous = selected; select(t.id); threads.insert(threads.begin(), std::move(t));
    if (!persist()) { threads.erase(threads.begin()); select(previous); }
    rebuild(); gtk_widget_grab_focus(input);
  }
  bool confirm(const char* text) {
    auto* d = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s", text);
    gtk_window_set_title(GTK_WINDOW(d), "Confirm conversation change");
    const auto result = gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d); return result == GTK_RESPONSE_OK;
  }
  void delete_thread() {
    if (!current() || selected == active || !confirm("Delete this conversation and its saved transcript?")) return;
    const auto old = threads;
    std::erase_if(threads, [&](const auto& t) { return t.id == selected; });
    if (!persist()) threads = old;
    select(threads.empty() ? "" : threads.front().id); rebuild();
  }
  void reset_context() {
    auto* t = current();
    if (!t || selected == active || !confirm("Start fresh context in this conversation? Earlier messages remain visible but will no longer be sent to the model.")) return;
    const auto old = *t; t->summary.clear(); t->compacted = t->messages.size() - t->messages.size() % 2;
    if (!persist()) *t = old;
    render();
  }
  void start(bool listing) {
    if (cancellable || !settings_ok || (!listing && !history_ok)) return;
    settings.model = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(model))));
    try { save_settings(settings_path, settings); }
    catch (const std::exception& e) { message(e.what()); return; }
    auto job = std::make_unique<Job>(); job->settings = settings; job->listing = listing;
    if (!listing) {
      auto* t = current(); if (!t) return;
      const auto text = contents(input);
      const bool pending = t->messages.size() % 2 != 0;
      if (text.empty() && !pending) return;
      if (text.size() > 256 * 1024) { message("Message exceeds 256 KiB. Shorten it before sending."); return; }
      if (settings.model.empty()) { message("Choose a model or refresh the model list first."); return; }
      const auto old = *t;
      // Retry keeps the last unanswered turn; editing replaces it without adding duplicate user turns.
      if (!text.empty()) {
        if (pending) t->messages.back().text = text;
        else t->messages.push_back({"user", text});
        if (t->messages.size() == 1) {
          auto* title = g_utf8_substring(text.c_str(), 0, std::min<glong>(60, g_utf8_strlen(text.c_str(), -1)));
          t->title = title; g_free(title);
          std::replace(t->title.begin(), t->title.end(), '\n', ' ');
        }
      }
      if (!persist()) { *t = old; return; }
      job->thread = *t; active = t->id;
      gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(input)), "", -1);
      rebuild();
    }
    cancellable = g_cancellable_new(); timed_out = false; sensitivity();
    deadline = g_timeout_add_seconds(settings.request_timeout, +[](gpointer p) -> gboolean {
      auto& s = *static_cast<Chat*>(p); s.deadline = 0; s.timed_out = true;
      g_cancellable_cancel(s.cancellable); return G_SOURCE_REMOVE;
    }, this);
    message(listing ? "Loading models…" : (tools_enabled(settings) ? "Waiting for the model… Enabled tools may run; results will appear in the transcript." : "Waiting for the model… Older context will be summarized if needed."));
    // Hold the application until completion, even if its last window closes.
    g_application_hold(G_APPLICATION(app));
    auto* task = g_task_new(nullptr, cancellable, +[](GObject*, GAsyncResult* result, gpointer data) {
      auto& self = *static_cast<Chat*>(data);
      auto* job = static_cast<Job*>(g_task_get_task_data(G_TASK(result)));
      const bool cancelled = g_cancellable_is_cancelled(self.cancellable);
      if (self.deadline) { g_source_remove(self.deadline); self.deadline = 0; }
      g_clear_object(&self.cancellable); self.active.clear();
      if (self.window) {
        if (cancelled) self.message(self.timed_out ? "Request timed out after " + std::to_string(job->settings.request_timeout) + " seconds. Increase the request timeout in Settings or retry." : "Stopped. The unanswered message is saved; Send / Retry tries again.");
        else if (!job->error.empty()) self.message(job->error + " The conversation has been preserved.");
        else if (job->listing) {
          gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(self.model));
          for (const auto& name : job->models) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(self.model), name.c_str());
          gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(self.model))), self.settings.model.c_str());
          self.message(job->models.empty() ? "No models reported. You can type a model name." : "Models loaded. Choose a model from the dropdown.");
        } else {
          auto it = std::find_if(self.threads.begin(), self.threads.end(), [&](const auto& t) { return t.id == job->thread.id; });
          if (it != self.threads.end()) *it = std::move(job->thread);
          if (self.persist()) self.message("Reply received.");
          self.render();
        }
        self.sensitivity();
      }
      g_application_release(G_APPLICATION(self.app));
    }, this);
    g_task_set_task_data(task, job.release(), +[](gpointer p) { delete static_cast<Job*>(p); });
    // Complete only after the worker has stopped touching its data.
    g_task_set_return_on_cancel(task, FALSE);
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer data, GCancellable* cancel) {
      auto& job = *static_cast<Job*>(data);
      try {
        if (job.listing) job.models = models(job.settings, cancel);
        else job.thread = complete(job.settings, std::move(job.thread), cancel);
      } catch (const std::exception& e) { job.error = e.what(); }
      g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
  }
  void configure() {
    auto* dialog = gtk_dialog_new_with_buttons("Chat Settings", GTK_WINDOW(window), GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_OK, nullptr);
    auto* notebook = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), notebook);
    auto* grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 10); gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), grid, gtk_label_new("Connection"));
    auto entry = [&](const char* label, const std::string& value, int row) {
      auto* w = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(w), value.c_str()); gtk_entry_set_width_chars(GTK_ENTRY(w), 36);
      auto* l = gtk_label_new_with_mnemonic(label); gtk_label_set_mnemonic_widget(GTK_LABEL(l), w); gtk_widget_set_halign(l, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1); gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1); return w;
    };
    auto* url = entry("Base _URL", settings.url, 0);
    auto* port = entry("_Port (0 = URL default)", std::to_string(settings.port), 1);
    auto* auth = gtk_check_button_new_with_mnemonic("Use _bearer token authentication");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auth), settings.authenticate);
    gtk_grid_attach(GTK_GRID(grid), auth, 0, 2, 2, 1);
    auto* token = entry("_Token", settings.token, 3); gtk_entry_set_visibility(GTK_ENTRY(token), FALSE);
    auto* context = entry("_Context tokens", std::to_string(settings.context), 4);
    auto* reply = entry("Maximum _reply tokens", std::to_string(settings.reply), 5);
    auto* timeout = entry("Request _deadline (seconds)", std::to_string(settings.request_timeout), 6);
    accessible(timeout, "Request timeout (seconds)");
    auto* note = gtk_label_new("Ollama: http://localhost/v1, port 11434\nllama.cpp: http://localhost/v1, port 8080\nSet context to the server's configured size. Estimates are conservative.\nTokens are saved in a private file; HTTP sends them without encryption.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(note), 58);
    gtk_grid_attach(GTK_GRID(grid), note, 0, 7, 2, 1);
    auto* tools = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(tools), 10); gtk_grid_set_column_spacing(GTK_GRID(tools), 12);
    gtk_container_set_border_width(GTK_CONTAINER(tools), 16);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tools, gtk_label_new("Tools"));
    const auto toggle = [&](const char* label, bool enabled, int row) {
      auto* w = gtk_check_button_new_with_mnemonic(label);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), enabled);
      gtk_grid_attach(GTK_GRID(tools), w, 0, row, 2, 1); return w;
    };
    auto* files = toggle("_File search (indexed filenames only)", settings.file_search, 0);
    auto* system = toggle("System _information (OS, CPU, memory, uptime)", settings.system_information, 1);
    auto* packages = toggle("_Mport package searches (read-only)", settings.package_queries, 2);
    auto* web = toggle("_Web search", settings.web_search, 3);
    auto* provider = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(provider), "brave", "Brave Search");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(provider), "searxng", "SearXNG");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(provider), settings.search_provider.c_str());
    accessible(provider, "Web search provider");
    gtk_grid_attach(GTK_GRID(tools), gtk_label_new("Search provider"), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(tools), provider, 1, 4, 1, 1);
    auto* search_url = gtk_entry_new(); accessible(search_url, "SearXNG search URL");
    gtk_entry_set_text(GTK_ENTRY(search_url), settings.search_url.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_url), "http://localhost:8080/search");
    gtk_grid_attach(GTK_GRID(tools), gtk_label_new("SearXNG search URL"), 0, 5, 1, 1);
    gtk_grid_attach(GTK_GRID(tools), search_url, 1, 5, 1, 1);
    auto* search_token = gtk_entry_new(); accessible(search_token, "Brave API key");
    gtk_entry_set_text(GTK_ENTRY(search_token), settings.search_token.c_str());
    gtk_entry_set_visibility(GTK_ENTRY(search_token), FALSE);
    gtk_grid_attach(GTK_GRID(tools), gtk_label_new("Brave API key"), 0, 6, 1, 1);
    gtk_grid_attach(GTK_GRID(tools), search_token, 1, 6, 1, 1);
    const auto limit = [&](const char* label, int value, int row) {
      auto* spin = gtk_spin_button_new_with_range(1, row == 7 ? 32 : 64, 1);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value); accessible(spin, label);
      gtk_grid_attach(GTK_GRID(tools), gtk_label_new(label), 0, row, 1, 1);
      gtk_grid_attach(GTK_GRID(tools), spin, 1, row, 1, 1); return spin;
    };
    auto* rounds = limit("Maximum tool rounds per reply", settings.tool_rounds, 7);
    auto* calls = limit("Maximum tool calls per reply", settings.tool_calls, 8);
    auto* disclosure = gtk_label_new("Tools are optional and run as your user. Local results are sent to the configured chat server. Web queries go to the selected search provider. API keys are saved in private local settings. Package searches do not install, remove, or update packages. Results and tool calls are saved with the conversation.");
    gtk_label_set_line_wrap(GTK_LABEL(disclosure), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(disclosure), 58);
    gtk_grid_attach(GTK_GRID(tools), disclosure, 0, 9, 2, 1);
    gtk_widget_show_all(dialog);
    while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
      try {
        auto integer = [](GtkWidget* w) {
          std::string text = gtk_entry_get_text(GTK_ENTRY(w)); std::size_t n{};
          int result = std::stoi(text, &n);
          if (n != text.size()) throw std::runtime_error("Use whole numbers for port, token limits and timeout.");
          return result;
        };
        Settings next = settings;
        next.url = gtk_entry_get_text(GTK_ENTRY(url)); next.port = integer(port);
        next.token = gtk_entry_get_text(GTK_ENTRY(token)); next.authenticate = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auth));
        next.context = integer(context); next.reply = integer(reply);
        next.request_timeout = integer(timeout);
        next.tool_rounds = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(rounds));
        next.tool_calls = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(calls));
        next.file_search = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(files));
        next.system_information = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(system));
        next.package_queries = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(packages));
        next.web_search = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(web));
        next.search_provider = gtk_combo_box_get_active_id(GTK_COMBO_BOX(provider));
        next.search_url = gtk_entry_get_text(GTK_ENTRY(search_url));
        next.search_token = gtk_entry_get_text(GTK_ENTRY(search_token));
        if (!next.authenticate) next.token.clear();
        save_settings(settings_path, next); settings = std::move(next);
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(model));
        gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(model))), settings.model.c_str());
        message("Settings saved. Refresh models to connect to this endpoint."); break;
      } catch (const std::exception& e) { error_dialog(e.what()); }
    }
    gtk_widget_destroy(dialog);
  }
  static GtkWidget* scroll(GtkWidget* child) {
    auto* s = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(s), child); return s;
  }
  GtkWidget* button(GtkWidget* box, const char* label, void (*fn)(Chat&)) {
    auto* w = gtk_button_new_with_mnemonic(label); gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(w), "action", reinterpret_cast<gpointer>(fn));
    g_signal_connect(w, "clicked", G_CALLBACK(+[](GtkWidget* w, gpointer p) {
      auto fn = reinterpret_cast<void (*)(Chat&)>(g_object_get_data(G_OBJECT(w), "action")); fn(*static_cast<Chat*>(p));
    }), this); return w;
  }
  void build() {
    window = gtk_application_window_new(app); gtk_window_set_title(GTK_WINDOW(window), "Midnight Chat");
    gtk_window_set_icon_name(GTK_WINDOW(window), "midnightbsd-chat");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 620);
    auto* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_container_set_border_width(GTK_CONTAINER(outer), 10);
    gtk_container_add(GTK_CONTAINER(window), outer);
    auto* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_pack_start(GTK_BOX(outer), bar, FALSE, FALSE, 0);
    auto* label = gtk_label_new_with_mnemonic("_Model:"); gtk_box_pack_start(GTK_BOX(bar), label, FALSE, FALSE, 0);
    model = gtk_combo_box_text_new_with_entry(); accessible(model, "Model");
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), model);
    gtk_entry_set_max_length(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(model))), 256);
    gtk_box_pack_start(GTK_BOX(bar), model, TRUE, TRUE, 0);
    refresh = button(bar, "_Refresh models", +[](Chat& s) { s.start(true); });
    settings_button = button(bar, "S_ettings", +[](Chat& s) { s.configure(); });
    auto* pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL); gtk_paned_set_position(GTK_PANED(pane), 210);
    gtk_box_pack_start(GTK_BOX(outer), pane, TRUE, TRUE, 0);
    auto* side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); gtk_paned_pack1(GTK_PANED(pane), side, FALSE, FALSE);
    button(side, "_New conversation", +[](Chat& s) { s.new_thread(); });
    list = gtk_list_box_new(); accessible(list, "Conversations");
    gtk_box_pack_start(GTK_BOX(side), scroll(list), TRUE, TRUE, 0);
    remove = button(side, "_Delete conversation", +[](Chat& s) { s.delete_thread(); });
    reset = button(side, "Reset conte_xt", +[](Chat& s) { s.reset_context(); });
    auto* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); gtk_widget_set_margin_start(right, 8);
    gtk_paned_pack2(GTK_PANED(pane), right, TRUE, FALSE);
    transcript = gtk_text_view_new(); accessible(transcript, "Conversation transcript");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(transcript), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(transcript), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(transcript), 12); gtk_text_view_set_right_margin(GTK_TEXT_VIEW(transcript), 12);
    gtk_box_pack_start(GTK_BOX(right), scroll(transcript), TRUE, TRUE, 0);
    input = gtk_text_view_new(); accessible(input, "Message"); gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(input), GTK_WRAP_WORD_CHAR);
    auto* input_scroll = scroll(input); gtk_widget_set_size_request(input_scroll, -1, 88);
    gtk_box_pack_start(GTK_BOX(right), input_scroll, FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(input, "Ctrl+Enter sends. Enter inserts a new line. An empty Send retries an unanswered message.");
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_pack_start(GTK_BOX(right), actions, FALSE, FALSE, 0);
    send = button(actions, "_Send / Retry", +[](Chat& s) { s.start(false); });
    stop = button(actions, "S_top", +[](Chat& s) { if (s.cancellable) g_cancellable_cancel(s.cancellable); });
    status = gtk_label_new("Configure a local server in Settings, then refresh models.");
    gtk_label_set_line_wrap(GTK_LABEL(status), TRUE); gtk_label_set_xalign(GTK_LABEL(status), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(status), 100); gtk_box_pack_start(GTK_BOX(outer), status, FALSE, FALSE, 0);
    g_signal_connect(list, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer p) {
      auto& s = *static_cast<Chat*>(p); if (s.rebuilding || !row) return;
      s.select(static_cast<const char*>(g_object_get_data(G_OBJECT(row), "thread"))); s.render();
    }), this);
    g_signal_connect(input, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer p) -> gboolean {
      if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)) {
        static_cast<Chat*>(p)->start(false); return TRUE;
      } return FALSE;
    }), this);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer p) {
      auto& s = *static_cast<Chat*>(p); s.window = nullptr;
      if (s.cancellable) g_cancellable_cancel(s.cancellable);
    }), this);
    try { settings = load_settings(settings_path); }
    catch (const std::exception& e) { settings_ok = false; message(std::string("Settings were not loaded or overwritten: ") + e.what()); }
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(model))), settings.model.c_str());
    try { threads = load_threads(history_path); }
    catch (const std::exception& e) { history_ok = false; message(std::string("History was not loaded or overwritten: ") + e.what()); }
    if (threads.empty() && history_ok) new_thread();
    else { if (!threads.empty()) selected = threads.front().id; rebuild(); }
    gtk_widget_show_all(window); gtk_widget_grab_focus(input); sensitivity();
  }
};
}
int main(int argc, char** argv) {
  Chat chat;
  chat.app = gtk_application_new("org.midnightbsd.Chat", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(chat.app, "activate", G_CALLBACK(+[](GtkApplication*, gpointer p) {
    auto& s = *static_cast<Chat*>(p);
    if (s.window) gtk_window_present(GTK_WINDOW(s.window)); else s.build();
  }), &chat);
  const int result = g_application_run(G_APPLICATION(chat.app), argc, argv);
  g_object_unref(chat.app); return result;
}
