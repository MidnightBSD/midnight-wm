#include "autostart.hpp"
#include "application_restore.hpp"

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>

namespace midnight {
namespace {
using DesktopInfo = std::unique_ptr<GDesktopAppInfo, decltype(&g_object_unref)>;

bool is_eligible(GDesktopAppInfo* info) {
  if (g_desktop_app_info_get_is_hidden(info) ||
      !g_desktop_app_info_get_show_in(info, nullptr)) return false;

  // NoDisplay controls menu visibility; it does not disable autostart.
  std::unique_ptr<char, decltype(&g_free)> try_exec(
      g_desktop_app_info_get_string(info, "TryExec"), g_free);
  if (try_exec && *try_exec) {
    std::unique_ptr<char, decltype(&g_free)> executable(
        g_find_program_in_path(try_exec.get()), g_free);
    if (!executable) return false;
  }
  return true;
}
} // namespace

std::vector<std::filesystem::path> find_autostart_entries(
    const std::vector<std::filesystem::path>& config_roots,
    std::span<const std::string> restored_executables,
    std::span<const std::string> restored_desktop_commands) {
  std::unordered_set<std::string> seen;
  std::vector<std::filesystem::path> entries;

  for (const auto& root : config_roots) {
    const auto directory = root / "autostart";
    std::error_code error;
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
      if (it->path().extension() == ".desktop" && it->is_regular_file(error))
        candidates.push_back(it->path());
    }
    std::ranges::sort(candidates);

    for (const auto& path : candidates) {
      // A higher priority file masks a lower priority file even when hidden
      // or not applicable to this desktop.
      if (!seen.insert(path.filename().string()).second) continue;
      DesktopInfo info(g_desktop_app_info_new_from_filename(path.c_str()), g_object_unref);
      if (info && is_eligible(info.get())) entries.push_back(path);
    }
  }
  std::map<std::string, std::size_t> restored_counts, entry_counts;
  for (const auto& executable : restored_executables) ++restored_counts[executable];
  std::map<std::filesystem::path, std::string> identities;
  for (const auto& path : entries) {
    DesktopInfo info(g_desktop_app_info_new_from_filename(path.c_str()), g_object_unref);
    const char* executable = info ? g_app_info_get_executable(G_APP_INFO(info.get())) : nullptr;
    if (!executable) continue;
    if (auto identity = resolve_session_executable(executable)) {
      identities[path] = *identity;
      ++entry_counts[*identity];
    }
  }
  std::map<std::string, std::size_t> restored_commands, entry_commands;
  std::map<std::filesystem::path, std::string> commands;
  for (const auto& command : restored_desktop_commands) ++restored_commands[command];
  for (const auto& path : entries) {
    DesktopInfo info(g_desktop_app_info_new_from_filename(path.c_str()), g_object_unref);
    const char* command = info ? g_app_info_get_commandline(G_APP_INFO(info.get())) : nullptr;
    if (command && *command) { commands[path] = command; ++entry_commands[command]; }
  }
  // Desktop relaunch matches the full installed command, so a wrapper or a
  // different profile sharing one executable cannot suppress another entry.
  // Only a unique direct executable match is unambiguous. Keep wrappers and
  // multiple autostart profiles eligible rather than guessing their identity.
  std::erase_if(entries, [&](const auto& path) {
    const auto found = identities.find(path);
    const auto command = commands.find(path);
    return (found != identities.end() && restored_counts[found->second] == 1 && entry_counts[found->second] == 1) ||
      (command != commands.end() && restored_commands[command->second] == 1 && entry_commands[command->second] == 1);
  });
  return entries;
}

void launch_autostart(std::span<const std::string> restored_executables,
    std::span<const std::string> restored_desktop_commands) {
  std::vector<std::filesystem::path> roots{g_get_user_config_dir()};
  for (auto dirs = g_get_system_config_dirs(); dirs && *dirs; ++dirs) roots.emplace_back(*dirs);

  for (const auto& path : find_autostart_entries(roots, restored_executables, restored_desktop_commands)) {
    DesktopInfo info(g_desktop_app_info_new_from_filename(path.c_str()), g_object_unref);
    if (!info) continue;
    GError* error = nullptr;
    if (!g_app_info_launch(G_APP_INFO(info.get()), nullptr, nullptr, &error)) {
      std::cerr << "Autostart " << path << ": "
                << (error ? error->message : "launch failed") << '\n';
    }
    if (error) g_error_free(error);
  }
}

void launch_xscreensaver(xcb_connection_t* connection, xcb_window_t root) {
  if (!connection || xcb_connection_has_error(connection)) return;
  const char name[] = "_SCREENSAVER_VERSION";
  auto* atom = xcb_intern_atom_reply(connection,
      xcb_intern_atom(connection, true, sizeof(name) - 1, name), nullptr);
  if (atom && atom->atom != XCB_ATOM_NONE) {
    auto* existing = xcb_get_property_reply(connection,
        xcb_get_property(connection, false, root, atom->atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 1), nullptr);
    const bool running = existing && xcb_get_property_value_length(existing) > 0;
    std::free(existing);
    std::free(atom);
    if (running) return;
  } else {
    std::free(atom);
  }

  char* program = g_find_program_in_path("xscreensaver");
  if (!program) return;
  const char* argv[]{program, "--no-splash", nullptr};
  GError* error = nullptr;
  auto* process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE, &error);
  g_free(program);
  if (!process) {
    std::cerr << "XScreenSaver: " << (error ? error->message : "launch failed") << '\n';
    g_clear_error(&error);
    return;
  }
  // GLib owns this direct child's wait; the session reaper must not consume it.
  g_subprocess_wait_async(process, nullptr, [](GObject* source, GAsyncResult* result, gpointer) {
    (void)g_subprocess_wait_finish(G_SUBPROCESS(source), result, nullptr);
    g_object_unref(source);
  }, nullptr);
}

} // namespace midnight
