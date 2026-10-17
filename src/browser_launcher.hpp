#pragma once

#include <gio/gio.h>

#include <expected>
#include <string>

namespace midnight {
class StartupNotification;

// Returns an owned GAppInfo. An explicit XDG HTTPS (or HTTP) default wins;
// without one, Raphael is the MidnightBSD fallback.
[[nodiscard]] GAppInfo* preferred_web_browser();
[[nodiscard]] std::expected<void, std::string> launch_web_browser(StartupNotification* startup);
} // namespace midnight
