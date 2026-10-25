// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "chat_core.hpp"
#include "chat_tools.hpp"
#include <gtk/gtk.h>
#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
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
struct Chat;
struct Job {
  Chat* owner{};
  Settings settings;
  Thread thread;
  bool listing{}, compacting{}, allow_all_searches{};
  ContextInfo context;
  std::vector<std::string> models;
  std::string error;
};
struct Chat {
  GtkApplication* app{};
  GtkWidget *window{}, *list{}, *transcript{}, *input{}, *status{}, *send{}, *stop{}, *model{}, *refresh{}, *settings_button{}, *remove{}, *reset{}, *compact_button{};
  std::filesystem::path settings_path{std::filesystem::path(g_get_user_config_dir()) / "midnightbsd-chat/settings.json"};
  std::filesystem::path history_path{std::filesystem::path(g_get_user_data_dir()) / "midnightbsd-chat/conversations.json"};
  Settings settings;
  ServerProfiles profiles;
  GtkWidget *server{}, *add_server{}, *remove_server{};
  bool selecting_server{};
  std::vector<Thread> threads;
  std::string selected, active;
  std::unordered_map<std::string, std::string> drafts;
  guint deadline{};
  bool timed_out{};
  GCancellable* cancellable{};
  bool history_ok{true}, settings_ok{true}, rebuilding{}, startup_failed{};
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
  static ServerProfile& chosen(ServerProfiles& profiles) {
    auto it = std::find_if(profiles.servers.begin(), profiles.servers.end(),
        [&](const auto& item) { return item.id == profiles.selected; });
    if (it == profiles.servers.end()) throw std::runtime_error("The selected server is missing.");
    return *it;
  }
  ServerProfiles edited_profiles() const {
    auto next = profiles;
    chosen(next).settings = settings;
    chosen(next).settings.model = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(model))));
    return next;
  }
  void refresh_servers() {
    selecting_server = true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(server));
    for (const auto& item : profiles.servers)
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(server), item.id.c_str(), item.name.c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(server), profiles.selected.c_str());
    selecting_server = false;
  }
  void commit_profiles(ServerProfiles next) {
    save_server_profiles(settings_path, next);
    profiles = std::move(next); settings = chosen(profiles).settings;
    refresh_servers();
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(model));
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(model))), settings.model.c_str());
    sensitivity();
  }
  void switch_server(const std::string& id) {
    if (id == profiles.selected) return;
    if (cancellable || !settings_ok) {
      selecting_server = true;
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(server), profiles.selected.c_str());
      selecting_server = false; return;
    }
    try {
      auto next = edited_profiles(); next.selected = id;
      commit_profiles(std::move(next));
      message("Server selected. Send uses this server for the current conversation; Refresh models lists its models.");
    } catch (const std::exception& error) {
      selecting_server = true;
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(server), profiles.selected.c_str());
      selecting_server = false; message(error.what());
    }
  }
  void delete_server() {
    if (cancellable || !settings_ok || profiles.servers.size() <= 1 ||
        !confirm("Remove this server and its saved credentials? Conversations will remain.", "Remove server")) return;
    try {
      auto next = profiles;
      std::erase_if(next.servers, [&](const auto& item) { return item.id == next.selected; });
      next.selected = next.servers.front().id;
      commit_profiles(std::move(next));
      message("Server removed. The current conversation and draft are unchanged.");
    } catch (const std::exception& error) { error_dialog(error.what()); }
  }
  void sensitivity() {
    if (!window) return;
    const bool busy = cancellable != nullptr;
    gtk_widget_set_sensitive(server, settings_ok && !busy);
    gtk_widget_set_sensitive(add_server, settings_ok && !busy && profiles.servers.size() < max_servers);
    gtk_widget_set_sensitive(remove_server, settings_ok && !busy && profiles.servers.size() > 1);
    gtk_widget_set_sensitive(send, history_ok && !busy && current());
    gtk_widget_set_sensitive(stop, busy);
    gtk_widget_set_sensitive(refresh, settings_ok && !busy);
    gtk_widget_set_sensitive(settings_button, settings_ok && !busy);
    gtk_widget_set_sensitive(model, settings_ok && !busy);
    gtk_widget_set_sensitive(remove, history_ok && current() && selected != active);
    gtk_widget_set_sensitive(reset, history_ok && current() && selected != active);
    gtk_widget_set_sensitive(compact_button, history_ok && !busy && current() &&
        current()->messages.size() >= current()->compacted + 4);
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
    if (current()) {
      auto text = contents(input);
      if (text.empty()) drafts.erase(selected);
      else drafts[selected] = std::move(text);
    }
    selected = id;
    const auto draft = drafts.find(id);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(input)),
        draft == drafts.end() ? "" : draft->second.c_str(), -1);
  }
  void new_thread() {
    if (!history_ok) return;
    char* id = g_uuid_string_random(); Thread t; t.id = id; g_free(id);
    auto previous = selected; select(t.id); threads.insert(threads.begin(), std::move(t));
    if (!persist()) { threads.erase(threads.begin()); select(previous); }
    rebuild(); gtk_widget_grab_focus(input);
  }
  // Runs on the worker thread: ask on the main thread whether a web search that
  // follows local tool results may proceed, and wait for the answer. Stop, the
  // request deadline and closing the window all decline.
  bool confirm_web_search(Job& job, GCancellable* cancel, const std::string& query) {
    if (job.allow_all_searches) return true;
    struct Ask {
      Chat* self; Job* job; GCancellable* cancel; std::string query;
      std::mutex mutex; std::condition_variable wake; bool done{}, allowed{};
    } ask{this, &job, cancel, query, {}, {}, false, false};
    g_main_context_invoke(nullptr, +[](gpointer p) -> gboolean {
      auto& a = *static_cast<Ask*>(p);
      int response = GTK_RESPONSE_CANCEL;
      if (a.self->window && !g_cancellable_is_cancelled(a.cancel)) {
        const std::string provider = a.job->settings.search_provider == "brave" ? "Brave Search" : "the configured SearXNG server";
        const auto text = "The model wants to search the web after reading local tool results in this reply. The query may include file names or system details and will be sent to " + provider + ".\n\nQuery:\n" + a.query;
        auto* d = gtk_message_dialog_new(GTK_WINDOW(a.self->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", text.c_str());
        gtk_window_set_title(GTK_WINDOW(d), "Allow web search?");
        gtk_dialog_add_buttons(GTK_DIALOG(d), "_Decline", GTK_RESPONSE_CANCEL, "Allow a_ll this reply", GTK_RESPONSE_ACCEPT, "_Allow", GTK_RESPONSE_OK, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
        const gulong handler = g_cancellable_connect(a.cancel, G_CALLBACK(+[](GCancellable*, gpointer dialog) {
          gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        }), d, nullptr);
        response = gtk_dialog_run(GTK_DIALOG(d));
        g_cancellable_disconnect(a.cancel, handler);
        gtk_widget_destroy(d);
      }
      if (response == GTK_RESPONSE_ACCEPT) a.job->allow_all_searches = true;
      { std::lock_guard lock(a.mutex); a.allowed = response == GTK_RESPONSE_OK || response == GTK_RESPONSE_ACCEPT; a.done = true; }
      a.wake.notify_all();
      return G_SOURCE_REMOVE;
    }, &ask);
    std::unique_lock lock(ask.mutex);
    ask.wake.wait(lock, [&] { return ask.done; });
    return ask.allowed;
  }
  bool confirm(const char* text, const char* title = "Confirm conversation change") {
    auto* d = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s", text);
    gtk_window_set_title(GTK_WINDOW(d), title);
    const auto result = gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d); return result == GTK_RESPONSE_OK;
  }
  void delete_thread() {
    if (!current() || selected == active || !confirm("Delete this conversation and its saved transcript?")) return;
    const auto old = threads;
    std::erase_if(threads, [&](const auto& t) { return t.id == selected; });
    if (!persist()) { threads = old; return; }
    drafts.erase(selected);
    select(threads.empty() ? "" : threads.front().id); rebuild();
  }
  void reset_context() {
    auto* t = current();
    if (!t || selected == active || !confirm("Start fresh context in this conversation? Earlier messages remain visible but will no longer be sent to the model.")) return;
    const auto old = *t; t->summary.clear(); t->compacted = t->messages.size() - t->messages.size() % 2;
    if (!persist()) *t = old;
    render();
  }
  void start(bool listing, bool compacting = false) {
    if (cancellable || !settings_ok || (!listing && !history_ok)) return;
    try {
      auto next = edited_profiles(); save_server_profiles(settings_path, next);
      profiles = std::move(next); settings = chosen(profiles).settings;
    }
    catch (const std::exception& e) { message(e.what()); return; }
    auto job = std::make_unique<Job>(); job->owner = this; job->settings = settings; job->listing = listing; job->compacting = compacting;
    if (compacting) {
      auto* t = current(); if (!t || t->messages.size() < t->compacted + 4) return;
      job->thread = *t; active = t->id;
    } else if (!listing) {
      auto* t = current(); if (!t) return;
      const auto text = contents(input);
      const bool pending = t->messages.size() % 2 != 0;
      if (text.empty() && !pending) return;
      if (!pending && t->messages.size() >= max_thread_messages) {
        message("Conversation message limit reached. Start a new conversation."); return;
      }
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
    message(compacting ? "Compacting older context… The full transcript will be retained." : listing ? "Loading models…" : (tools_enabled(settings) ? "Waiting for the model… Enabled tools may run; results will appear in the transcript." : "Waiting for the model… Older context will be summarized if needed."));
    // Hold the application until completion, even if its last window closes.
    g_application_hold(G_APPLICATION(app));
    auto* task = g_task_new(nullptr, cancellable, +[](GObject*, GAsyncResult* result, gpointer data) {
      auto& self = *static_cast<Chat*>(data);
      auto* job = static_cast<Job*>(g_task_get_task_data(G_TASK(result)));
      const bool cancelled = g_cancellable_is_cancelled(self.cancellable);
      if (self.deadline) { g_source_remove(self.deadline); self.deadline = 0; }
      g_clear_object(&self.cancellable); self.active.clear();
      if (self.window) {
        if (cancelled) self.message(self.timed_out ? "Request timed out after " + std::to_string(job->settings.request_timeout) + " seconds. Increase the request timeout in Settings or retry." : job->compacting ? "Compaction stopped. Earlier context and the transcript are preserved." : "Stopped. The unanswered message is saved; Send / Retry tries again.");
        else if (!job->error.empty()) self.message(job->error + " The conversation has been preserved.");
        else if (job->listing) {
          gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(self.model));
          for (const auto& name : job->models) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(self.model), name.c_str());
          gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(self.model))), self.settings.model.c_str());
          self.message(job->models.empty() ? "No models reported. You can type a model name." : "Models loaded. Choose a model from the dropdown.");
        } else {
          auto it = std::find_if(self.threads.begin(), self.threads.end(), [&](const auto& t) { return t.id == job->thread.id; });
          const bool summarized = it != self.threads.end() && job->thread.compacted > it->compacted;
          if (it != self.threads.end()) *it = std::move(job->thread);
          if (self.persist()) self.message(std::string(job->compacting ? "Context compacted. " : summarized ? "Reply received; older context compacted. " : "Reply received. ") +
              "Context: " + std::to_string(job->context.tokens) + " tokens, " + job->context.source + ".");
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
        else if (job.compacting) job.thread = compact(job.settings, std::move(job.thread), cancel, &job.context);
        else job.thread = complete(job.settings, std::move(job.thread), cancel, &job.context,
            [&job, cancel](const std::string& query) { return job.owner->confirm_web_search(job, cancel, query); });
      } catch (const std::exception& e) { job.error = e.what(); }
      g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
  }
  void configure(bool adding = false) {
    if (cancellable || !settings_ok || (adding && profiles.servers.size() >= max_servers)) return;
    auto edited = edited_profiles();
    const auto initial = adding ? Settings{} : chosen(edited).settings;
    std::string initial_name;
    if (adding) {
      int index = 1;
      do { initial_name = "Server " + std::to_string(index++); }
      while (std::any_of(profiles.servers.begin(), profiles.servers.end(),
          [&](const auto& item) { return item.name == initial_name; }));
    } else initial_name = chosen(edited).name;
    auto* dialog = gtk_dialog_new_with_buttons(adding ? "Add server" : "Chat Settings", GTK_WINDOW(window), GTK_DIALOG_MODAL,
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
    auto* name = entry("Server _name", initial_name, 0);
    gtk_entry_set_max_length(GTK_ENTRY(name), 64);
    accessible(name, "Server name");
    auto* url = entry("Base _URL", initial.url, 1);
    auto* port = entry("_Port (0 = URL default)", std::to_string(initial.port), 2);
    auto* auth = gtk_check_button_new_with_mnemonic("Use _bearer token authentication");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auth), initial.authenticate);
    gtk_grid_attach(GTK_GRID(grid), auth, 0, 3, 2, 1);
    auto* token = entry("_Token", initial.token, 4); gtk_entry_set_visibility(GTK_ENTRY(token), FALSE);
    auto* context = entry("_Context tokens", std::to_string(initial.context), 5);
    auto* reply = entry("Maximum _reply tokens", std::to_string(initial.reply), 6);
    auto* timeout = entry("Request _deadline (seconds)", std::to_string(initial.request_timeout), 7);
    accessible(timeout, "Request timeout (seconds)");
    auto* note = gtk_label_new("Ollama: http://localhost/v1, port 11434\nllama.cpp: http://localhost/v1, port 8080\nContext is a fallback and cap; reported smaller limits apply automatically.\nOlder turns compact before the budget is full. Estimates are conservative.\nTokens are saved in a private file; HTTP sends them without encryption.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(note), 58);
    gtk_grid_attach(GTK_GRID(grid), note, 0, 8, 2, 1);
    auto* tools = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(tools), 10); gtk_grid_set_column_spacing(GTK_GRID(tools), 12);
    gtk_container_set_border_width(GTK_CONTAINER(tools), 16);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tools, gtk_label_new("Tools"));
    const auto toggle = [&](const char* label, bool enabled, int row) {
      auto* w = gtk_check_button_new_with_mnemonic(label);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), enabled);
      gtk_grid_attach(GTK_GRID(tools), w, 0, row, 2, 1); return w;
    };
    auto* files = toggle("_File search (indexed filenames only)", initial.file_search, 0);
    auto* system = toggle("System _information (OS, CPU, memory, uptime)", initial.system_information, 1);
    auto* packages = toggle("_Mport package searches (read-only)", initial.package_queries, 2);
    auto* web = toggle("_Web search", initial.web_search, 3);
    auto* provider = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(provider), "brave", "Brave Search");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(provider), "searxng", "SearXNG");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(provider), initial.search_provider.c_str());
    accessible(provider, "Web search provider");
    gtk_grid_attach(GTK_GRID(tools), gtk_label_new("Search provider"), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(tools), provider, 1, 4, 1, 1);
    auto* search_url = gtk_entry_new(); accessible(search_url, "SearXNG search URL");
    gtk_entry_set_text(GTK_ENTRY(search_url), initial.search_url.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_url), "http://localhost:8080/search");
    gtk_grid_attach(GTK_GRID(tools), gtk_label_new("SearXNG search URL"), 0, 5, 1, 1);
    gtk_grid_attach(GTK_GRID(tools), search_url, 1, 5, 1, 1);
    auto* search_token = gtk_entry_new(); accessible(search_token, "Brave API key");
    gtk_entry_set_text(GTK_ENTRY(search_token), initial.search_token.c_str());
    gtk_entry_set_visibility(GTK_ENTRY(search_token), FALSE);
    gtk_grid_attach(GTK_GRID(tools), gtk_label_new("Brave API key"), 0, 6, 1, 1);
    gtk_grid_attach(GTK_GRID(tools), search_token, 1, 6, 1, 1);
    const auto limit = [&](const char* label, int value, int row) {
      auto* spin = gtk_spin_button_new_with_range(1, row == 7 ? 32 : 64, 1);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value); accessible(spin, label);
      gtk_grid_attach(GTK_GRID(tools), gtk_label_new(label), 0, row, 1, 1);
      gtk_grid_attach(GTK_GRID(tools), spin, 1, row, 1, 1); return spin;
    };
    auto* rounds = limit("Maximum tool rounds per reply", initial.tool_rounds, 7);
    auto* calls = limit("Maximum tool calls per reply", initial.tool_calls, 8);
    auto* disclosure = gtk_label_new("Tools are optional and run as your user. Local results are sent to the configured chat server. Web queries go to the selected search provider; a web search that follows local tool results in the same reply asks for your confirmation first. API keys are saved in private local settings. Package searches do not install, remove, or update packages. Results and tool calls are saved with the conversation.");
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
        Settings next = initial;
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
        if (cleartext_credentials(next) &&
            !confirm("This server uses http:// to a remote host, so the bearer token will be sent unencrypted on every request. Use an https:// base URL for remote servers.\n\nSave anyway?", "Unencrypted credentials"))
          continue;
        auto updated = edited;
        if (adding) {
          auto item = new_server_profile(gtk_entry_get_text(GTK_ENTRY(name)));
          item.settings = std::move(next); updated.selected = item.id;
          updated.servers.push_back(std::move(item));
        } else {
          auto& item = chosen(updated);
          item.name = gtk_entry_get_text(GTK_ENTRY(name)); item.settings = std::move(next);
        }
        commit_profiles(std::move(updated));
        message("Server settings saved. Refresh models to connect to this server."); break;
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
      auto& self = *static_cast<Chat*>(p);
      auto fn = reinterpret_cast<void (*)(Chat&)>(g_object_get_data(G_OBJECT(w), "action"));
      try { fn(self); }
      catch (const std::exception& error) { self.error_dialog(error.what()); }
    }), this); return w;
  }
  void build() {
    window = gtk_application_window_new(app); gtk_window_set_title(GTK_WINDOW(window), "Midnight Chat");
    gtk_window_set_icon_name(GTK_WINDOW(window), "midnightbsd-chat");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 620);
    auto* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_container_set_border_width(GTK_CONTAINER(outer), 10);
    gtk_container_add(GTK_CONTAINER(window), outer);
    auto* servers = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(outer), servers, FALSE, FALSE, 0);
    auto* server_label = gtk_label_new_with_mnemonic("Ser_ver:");
    gtk_box_pack_start(GTK_BOX(servers), server_label, FALSE, FALSE, 0);
    server = gtk_combo_box_text_new(); accessible(server, "Server");
    gtk_label_set_mnemonic_widget(GTK_LABEL(server_label), server);
    auto* cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(server));
    for (auto* cell = cells; cell; cell = cell->next)
      g_object_set(cell->data, "ellipsize", PANGO_ELLIPSIZE_END, "max-width-chars", 28, nullptr);
    g_list_free(cells);
    gtk_box_pack_start(GTK_BOX(servers), server, TRUE, TRUE, 0);
    add_server = button(servers, "_Add server…", +[](Chat& s) { s.configure(true); });
    remove_server = button(servers, "Remove server…", +[](Chat& s) { s.delete_server(); });
    g_signal_connect(server, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer data) {
      auto& self = *static_cast<Chat*>(data);
      const char* id = gtk_combo_box_get_active_id(combo);
      if (!self.selecting_server && id) self.switch_server(id);
    }), this);
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
    compact_button = button(side, "_Compact now", +[](Chat& s) { s.start(false, true); });
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
    try { profiles = load_server_profiles(settings_path); settings = chosen(profiles).settings; refresh_servers(); }
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
    try {
      if (s.window) gtk_window_present(GTK_WINDOW(s.window)); else s.build();
    } catch (const std::exception& error) {
      g_printerr("Could not open Midnight Chat: %s\n", error.what());
      s.startup_failed = true; g_application_quit(G_APPLICATION(s.app));
    }
  }), &chat);
  const int result = g_application_run(G_APPLICATION(chat.app), argc, argv);
  g_object_unref(chat.app); return chat.startup_failed ? 1 : result;
}
