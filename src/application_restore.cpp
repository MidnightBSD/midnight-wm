// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "application_restore.hpp"
#include <glib.h>
#include <gio/gdesktopappinfo.h>
#include <set>
#include <map>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <memory>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace midnight {
std::optional<std::string> resolve_session_executable(const std::string& executable) {
  if (executable.empty() || executable.find('\0') != std::string::npos) return std::nullopt;
  const auto path = std::filesystem::path(executable);
  if (!path.is_absolute() && path.has_parent_path()) return std::nullopt;
  std::unique_ptr<char, decltype(&g_free)> found(g_find_program_in_path(executable.c_str()), g_free);
  if (!found) return std::nullopt;
  std::error_code error;
  const auto resolved = std::filesystem::canonical(found.get(), error);
  if (error || access(resolved.c_str(), X_OK) != 0) return std::nullopt;
  return resolved.string();
}
ApplicationRestorer::~ApplicationRestorer() { tick(); }
void ApplicationRestorer::tick() {
  for (auto it = children_.begin(); it != children_.end();) {
    int status{};
    const auto result = waitpid(*it, &status, WNOHANG);
    if (result == *it || (result < 0 && errno == ECHILD)) it = children_.erase(it);
    else ++it;
  }
}
std::vector<std::string> ApplicationRestorer::restore(const ApplicationCheckpoint& checkpoint, bool reopen_desktop_applications) {
  std::vector<std::string> errors;
  if (started_) return errors;
  started_ = true;
  if (checkpoint.applications.size() > 128) return {"Too many applications in restore plan"};
  for (const auto& application : checkpoint.applications) {
    if (auto valid = validate_application_restart(application); !valid) {
      errors.push_back(valid.error());
      continue;
    }
    if (application.style == RestartStyle::never) continue;
    const auto executable = resolve_session_executable(application.arguments.front());
    if (!executable) { errors.push_back("Missing restart executable: " + application.arguments.front()); continue; }
    std::vector<char*> arguments;
    for (const auto& value : application.arguments) arguments.push_back(const_cast<char*>(value.c_str()));
    arguments.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) { errors.push_back(std::strerror(error)); continue; }
    const auto* directory = application.directory.empty() ? g_get_home_dir() : application.directory.c_str();
    error = posix_spawn_file_actions_addchdir_np(&actions, directory);
    if (!error) error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    posix_spawnattr_t attributes;
    const int initialized = posix_spawnattr_init(&attributes);
    if (!error) error = initialized;
    if (!error) {
      sigset_t defaults, mask;
      sigemptyset(&defaults);
      for (int signal : {SIGINT, SIGTERM, SIGUSR1, SIGPIPE}) sigaddset(&defaults, signal);
      sigemptyset(&mask);
      error = posix_spawnattr_setsigdefault(&attributes, &defaults);
      if (!error) error = posix_spawnattr_setsigmask(&attributes, &mask);
      if (!error) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
      pid_t child{};
      // posix_spawn (not spawnp/execvp) never retries ENOEXEC via a shell.
      if (!error) error = posix_spawn(&child, executable->c_str(), &actions, &attributes, arguments.data(), environ);
      if (!error) { children_.push_back(child); launched_.push_back(*executable); }
    }
    if (!initialized) posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (error) errors.push_back("Restart " + application.program + ": " + std::strerror(error));
  }
  if (reopen_desktop_applications) {
    if (checkpoint.desktop_entries.size() > 128) errors.push_back("Too many desktop applications");
    else {
      std::set<std::string> xsmp_executables;
      for (const auto& app : checkpoint.applications)
        if (!app.arguments.empty())
          if (auto executable = resolve_session_executable(app.arguments.front())) xsmp_executables.insert(*executable);
      using Info = std::unique_ptr<GDesktopAppInfo, decltype(&g_object_unref)>;
      std::map<std::string, Info> entries;
      std::map<std::string, std::string> executables;
      std::map<std::string, std::size_t> counts;
      for (const auto& id : checkpoint.desktop_entries) {
        if (!valid_desktop_application_id(id) || entries.contains(id)) continue;
        Info info(g_desktop_app_info_new(id.c_str()), g_object_unref);
        if (!info || !g_app_info_should_show(G_APP_INFO(info.get()))) continue;
        const char* program = g_app_info_get_executable(G_APP_INFO(info.get()));
        const auto executable = program ? resolve_session_executable(program) : std::nullopt;
        if (!executable || xsmp_executables.contains(*executable)) continue;
        ++counts[*executable];
        executables[id] = *executable;
        entries.emplace(id, std::move(info));
      }
      for (const auto& [id, info] : entries) {
        const auto& executable = executables[id];
        if (counts[executable] != 1) continue;
        GError* error{};
        if (g_app_info_launch(G_APP_INFO(info.get()), nullptr, nullptr, &error)) {
          const char* command = g_app_info_get_commandline(G_APP_INFO(info.get()));
          if (command && *command) desktop_commands_.emplace_back(command);
        } else errors.push_back("Reopen " + id + ": " + (error ? error->message : "launch failed"));
        if (error) g_error_free(error);
      }
    }
  }
  return errors;
}
std::expected<std::optional<ApplicationCheckpoint>, std::string> take_application_restore_plan(bool enabled) {
  const char* plan = g_getenv("MIDNIGHT_WM_RESTORE_PLAN");
  const char* owner = g_getenv("MIDNIGHT_WM_RESTORE_MANAGER");
  const char* manager = g_getenv("SESSION_MANAGER");
  const std::string path = plan ? plan : "";
  const bool owned = owner && manager && *manager && std::strcmp(owner, manager) == 0;
  g_unsetenv("MIDNIGHT_WM_RESTORE_PLAN");
  g_unsetenv("MIDNIGHT_WM_RESTORE_MANAGER");
  if (!enabled || !owned || path.empty()) return std::nullopt;
  if (!std::filesystem::path(path).is_absolute()) return std::unexpected("Invalid companion restore plan path");
  return ApplicationSessionStore(path).load();
}
} // namespace midnight
