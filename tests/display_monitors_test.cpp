#include "display_monitors.hpp"

#include <iostream>
#include <vector>

namespace {
bool check(const midnight::DisplayMonitor& actual, const midnight::DisplayMonitor& expected,
           const char* case_name) {
  if (actual == expected) return true;
  std::cerr << case_name << ": selected (" << actual.x << ',' << actual.y << ','
            << actual.width << ',' << actual.height << ") instead of ("
            << expected.x << ',' << expected.y << ',' << expected.width << ','
            << expected.height << ")\n";
  return false;
}
}

int main() {
  using midnight::DisplayMonitor;
  using midnight::choose_display_monitor;

  const DisplayMonitor empty{0, 0, 0, 0, true};
  bool valid = check(choose_display_monitor({empty}, 640, 360), empty,
                     "zero-size monitor does not trap");
  const DisplayMonitor left{0, 0, 1920, 1080, true};
  const DisplayMonitor right{1920, 0, 3840, 2160, false};
  const std::vector<DisplayMonitor> side_by_side{left, right};
  valid &= check(choose_display_monitor(side_by_side, 100, 100), left, "left point");
  valid &= check(choose_display_monitor(side_by_side, 4500, 1500), right, "right point");
  valid &= check(choose_display_monitor(side_by_side, 1919, 100), left, "left boundary");
  valid &= check(choose_display_monitor(side_by_side, 1920, 100), right, "right boundary");

  const DisplayMonitor offset_right{1920, 400, 1920, 1080, false};
  const std::vector<DisplayMonitor> offset{left, offset_right};
  valid &= check(choose_display_monitor(offset, 2300, 100), offset_right,
                 "nearest monitor above offset output");

  const DisplayMonitor left_with_gap{0, 0, 1600, 900, false};
  const DisplayMonitor right_with_gap{1999, 0, 1600, 900, true};
  const std::vector<DisplayMonitor> gap{left_with_gap, right_with_gap};
  valid &= check(choose_display_monitor(gap, 1700, 450), left_with_gap,
                 "nearest monitor in gap");
  valid &= check(choose_display_monitor(gap, 1799, 450), right_with_gap,
                 "equal-distance gap chooses primary");

  const DisplayMonitor right_primary{1920, 0, 3840, 2160, true};
  const DisplayMonitor left_secondary{0, 0, 1920, 1080, false};
  valid &= check(choose_display_monitor({right_primary, left_secondary}, 100, 100),
                 left_secondary, "primary swap keeps geometric selection");
  valid &= check(choose_display_monitor({right_primary, left_secondary}, 4500, 1500),
                 right_primary, "primary swap chooses right for right point");

  const DisplayMonitor only{1000, 200, 1280, 720, true};
  valid &= check(choose_display_monitor({only}, -300, -300), only,
                 "disconnected output recovers to remaining output");
  valid &= check(choose_display_monitor({}, 100, 100), {0, 0, 1, 1, true},
                 "empty monitor fallback");
  using midnight::fullscreen_monitor_area;
  const auto span = fullscreen_monitor_area(side_by_side, {0, 1, 0, 1});
  valid &= span && check(*span, {0, 0, 5760, 2160}, "fullscreen span");
  const auto shifted = fullscreen_monitor_area({{-1920, -400, 1920, 1080}, left}, {0, 1, 0, 1});
  valid &= shifted && check(*shifted, {-1920, -400, 3840, 1480}, "negative fullscreen origin");
  valid &= !fullscreen_monitor_area(side_by_side, {0, 0, 1, 0}); // empty rectangle
  valid &= !fullscreen_monitor_area(side_by_side, {0, 0, 0, 99});
  valid &= !fullscreen_monitor_area({}, {0, 0, 0, 0});
  valid &= !fullscreen_monitor_area({{0, 0, 65536, 720}}, {0, 0, 0, 0});
  valid &= !fullscreen_monitor_area({{32768, 0, 1280, 720}}, {0, 0, 0, 0});
  return valid ? 0 : 1;
}
