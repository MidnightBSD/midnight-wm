#pragma once

#include <filesystem>
#include <vector>
#include <span>
#include <string>
#include <xcb/xcb.h>

namespace midnight {

// Search roots are ordered from highest to lowest priority. Each root is an
// XDG configuration directory, not its autostart subdirectory.
std::vector<std::filesystem::path> find_autostart_entries(
    const std::vector<std::filesystem::path>& config_roots,
    std::span<const std::string> restored_executables = {},
    std::span<const std::string> restored_desktop_commands = {});

// Launch eligible session applications after the window manager is ready.
// Match XSMP by unique direct executable; match desktop relaunch by unique
// complete installed command. Never execute these command strings ourselves.
void launch_autostart(std::span<const std::string> restored_executables = {},
    std::span<const std::string> restored_desktop_commands = {});

// Start the optional saver after other autostart entries have had time to
// register. An already running saver belongs to its original owner.
void launch_xscreensaver(xcb_connection_t* connection, xcb_window_t root);

} // namespace midnight
