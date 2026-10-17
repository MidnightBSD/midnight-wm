#pragma once

#include <expected>
#include <filesystem>
#include <string>

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

// Recheck trust immediately before launching. A desktop entry is trusted when
// it is a regular, user-owned, owner-executable file directly on the Desktop.
[[nodiscard]] std::expected<void, std::string> launch_desktop_entry(
    const std::filesystem::path& path, const std::filesystem::path& desktop_directory,
    StartupNotification* startup = nullptr);

} // namespace midnight
