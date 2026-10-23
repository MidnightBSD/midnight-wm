// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
// Exercise the real GTK conversation actions without a network dependency.
#define main midnight_chat_application_main
#include "../src/chat.cpp"
#undef main
#include <iostream>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void edit(Chat& chat, const std::string& text) {
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(chat.input)), text.c_str(), text.size());
}
void remove(Chat& chat, int response = GTK_RESPONSE_OK) {
  g_idle_add(+[](gpointer data) -> gboolean {
    auto* windows = gtk_window_list_toplevels();
    bool answered = false;
    for (auto* item = windows; item; item = item->next) {
      auto* widget = GTK_WIDGET(item->data);
      if (GTK_IS_DIALOG(widget) && gtk_widget_get_visible(widget)) {
        gtk_dialog_response(GTK_DIALOG(widget), GPOINTER_TO_INT(data));
        answered = true;
      }
    }
    g_list_free(windows);
    return answered ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
  }, GINT_TO_POINTER(response));
  chat.delete_thread();
}
}
int main(int argc, char** argv) {
  try {
    gtk_init(&argc, &argv);
    Chat chat;
    chat.app = gtk_application_new("org.midnightbsd.ChatLifecycleTest", G_APPLICATION_NON_UNIQUE);
    require(g_application_register(G_APPLICATION(chat.app), nullptr, nullptr), "Application registration failed");
    chat.build();
    const auto kept = chat.selected;
    const std::string kept_draft = "Keep this draft";
    const std::string large_draft(256 * 1024, 'D');
    edit(chat, kept_draft);
    chat.new_thread();
    const auto deleted = chat.selected;
    edit(chat, large_draft);
    chat.select(kept);
    chat.select(deleted);
    remove(chat, GTK_RESPONSE_CANCEL);
    require(chat.threads.size() == 2 && contents(chat.input) == large_draft,
        "Cancelled deletion lost a conversation or draft");
    remove(chat);
    require(chat.threads.size() == 1 && !chat.drafts.contains(deleted), "Deleted draft remains cached");
    require(chat.selected == kept && contents(chat.input) == kept_draft, "Deletion lost another conversation's draft");

    // A directory at the destination reliably forces an atomic save to fail,
    // including when the test runs as root. Preserve selection and draft.
    const auto original_path = chat.history_path;
    chat.history_path = original_path.parent_path() / "blocked";
    std::filesystem::create_directory(chat.history_path);
    remove(chat);
    require(chat.threads.size() == 1 && chat.selected == kept && contents(chat.input) == kept_draft,
        "Failed deletion lost the selected conversation or input");
    for (int i = 0; i < 5; ++i) chat.new_thread();
    require(chat.threads.size() == 1 && chat.selected == kept && chat.drafts.size() <= 1,
        "Failed creation retained orphan drafts");
    chat.history_path = original_path;

    // Deleted drafts must not accumulate even while another conversation lives.
    for (int i = 0; i < 100; ++i) {
      chat.new_thread();
      const auto id = chat.selected;
      edit(chat, large_draft);
      chat.select(kept);
      chat.select(id);
      remove(chat);
      require(chat.threads.size() == 1 && !chat.drafts.contains(id) && chat.drafts.size() <= 1,
          "Repeated deletion retained drafts for removed conversations");
      require(contents(chat.input) == kept_draft, "Repeated deletion changed a surviving draft");
    }
    remove(chat);
    require(chat.threads.empty() && chat.selected.empty() && chat.drafts.empty() && contents(chat.input).empty(),
        "Deleting the final conversation left draft state behind");
    gtk_widget_destroy(chat.window);
    while (g_main_context_iteration(nullptr, FALSE)) {}
    g_object_unref(chat.app);
    std::cout << "PASS: draft retention, cancelled/failed deletion, failed creation, and last-conversation cleanup\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
