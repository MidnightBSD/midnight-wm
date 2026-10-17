#pragma once

#include <gio/gio.h>

#include <expected>
#include <string>

namespace midnight {
class StartupNotification;

// Returns an owned GAppInfo. XDG terminal preferences and TERMINAL take
// precedence over the xterm fallback.
[[nodiscard]] GAppInfo* preferred_terminal();
[[nodiscard]] std::expected<void, std::string> launch_terminal(StartupNotification* startup);
} // namespace midnight
