#pragma once

#include <filesystem>
#include <vector>
#include <span>
#include <string>

namespace midnight {

// Search roots are ordered from highest to lowest priority. Each root is an
// XDG configuration directory, not its autostart subdirectory.
std::vector<std::filesystem::path> find_autostart_entries(
    const std::vector<std::filesystem::path>& config_roots,
    std::span<const std::string> restored_executables = {});

// Launch eligible session applications after the window manager is ready.
// Skip only unique direct executable matches among successful restores.
void launch_autostart(std::span<const std::string> restored_executables = {});

} // namespace midnight
