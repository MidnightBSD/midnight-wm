#pragma once

#include <filesystem>
#include <vector>

namespace midnight {

// Search roots are ordered from highest to lowest priority. Each root is an
// XDG configuration directory, not its autostart subdirectory.
std::vector<std::filesystem::path> find_autostart_entries(
    const std::vector<std::filesystem::path>& config_roots);

// Launch eligible session applications after the window manager is ready.
void launch_autostart();

} // namespace midnight
