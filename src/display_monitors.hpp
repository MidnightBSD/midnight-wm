#pragma once

#include <xcb/xcb.h>
#include <xcb/randr.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

namespace midnight {

struct DisplayMonitor {
  int x{}, y{}, width{}, height{};
  bool primary{};
  xcb_atom_t name{};
  bool operator==(const DisplayMonitor&) const = default;
};

inline DisplayMonitor choose_display_monitor(const std::vector<DisplayMonitor>& monitors,
                                             int x, int y) {
  if (monitors.empty()) return {0, 0, 1, 1, true};
  const DisplayMonitor* closest = nullptr;
  std::int64_t best_distance = std::numeric_limits<std::int64_t>::max();
  for (const auto& monitor : monitors) {
    const int near_x = std::clamp(x, monitor.x, monitor.x + monitor.width - 1);
    const int near_y = std::clamp(y, monitor.y, monitor.y + monitor.height - 1);
    const auto dx = std::int64_t(x) - near_x;
    const auto dy = std::int64_t(y) - near_y;
    const auto distance = dx * dx + dy * dy;
    if (!closest || distance < best_distance || (distance == best_distance && monitor.primary)) {
      closest = &monitor;
      best_distance = distance;
    }
  }
  return *closest;
}

inline std::vector<DisplayMonitor> query_display_monitors(xcb_connection_t* connection,
                                                           xcb_screen_t* screen) {
  std::vector<DisplayMonitor> result;
  const auto* extension = xcb_get_extension_data(connection, &xcb_randr_id);
  if (extension && extension->present) {
    auto cookie = xcb_randr_get_monitors(connection, screen->root, 1);
    std::unique_ptr<xcb_randr_get_monitors_reply_t, decltype(&std::free)> reply(
        xcb_randr_get_monitors_reply(connection, cookie, nullptr), std::free);
    if (reply) {
      auto iterator = xcb_randr_get_monitors_monitors_iterator(reply.get());
      for (; iterator.rem; xcb_randr_monitor_info_next(&iterator)) {
        const auto* info = iterator.data;
        if (info && info->width && info->height)
          result.push_back({info->x, info->y, info->width, info->height,
                            info->primary != 0, info->name});
      }
    }
  }
  if (result.empty()) {
    std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry(
        xcb_get_geometry_reply(connection, xcb_get_geometry(connection, screen->root), nullptr),
        std::free);
    result.push_back({0, 0, geometry ? geometry->width : screen->width_in_pixels,
                      geometry ? geometry->height : screen->height_in_pixels, true});
  }
  if (std::none_of(result.begin(), result.end(), [](const auto& monitor) { return monitor.primary; }))
    result.front().primary = true;
  return result;
}

} // namespace midnight
