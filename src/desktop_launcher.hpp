#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>
#include <gio/gio.h>

namespace midnight {
class StartupNotification;

struct DesktopLauncherInfo {
  std::string name;
  std::string icon;
  bool trusted{};
};

// Read the visible metadata even for an untrusted entry, so the shell can draw
// it without making it executable. Only Type=Application entries are accepted.
[[nodiscard]] std::expected<DesktopLauncherInfo, std::string> desktop_launcher_info(
    const std::filesystem::path& path, const std::filesystem::path& desktop_directory);

struct DesktopEntryAction {
  std::string id;
  std::string name;
  std::string icon;
};

// For applications selected from the installed XDG application registry. Callers
// must use the path API below for arbitrary Desktop or downloaded files.
[[nodiscard]] std::vector<DesktopEntryAction> desktop_entry_actions(GAppInfo* app);
[[nodiscard]] std::expected<void, std::string> launch_desktop_action(
    GAppInfo* app, const std::string& action, GAppLaunchContext* context = nullptr);

// Metadata only: listing actions never grants permission to launch an entry.
[[nodiscard]] std::expected<std::vector<DesktopEntryAction>, std::string> desktop_launcher_actions(
    const std::filesystem::path& path, const std::filesystem::path& desktop_directory);

// Recheck both Desktop trust and action membership at launch time. GIO handles
// Exec field codes and D-Bus activation; no command is passed to a shell.
// GIO's action API reports no execution errors after dispatching the request.
[[nodiscard]] std::expected<void, std::string> launch_desktop_entry_action(
    const std::filesystem::path& path, const std::filesystem::path& desktop_directory,
    const std::string& action, GAppLaunchContext* context = nullptr);

// Recheck trust immediately before launching. A desktop entry is trusted when
// it is a regular, user-owned, owner-executable file directly on the Desktop.
[[nodiscard]] std::expected<void, std::string> launch_desktop_entry(
    const std::filesystem::path& path, const std::filesystem::path& desktop_directory,
    StartupNotification* startup = nullptr);

} // namespace midnight
