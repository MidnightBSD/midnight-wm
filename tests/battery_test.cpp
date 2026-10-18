/*
 * Copyright (c) 2026 Lucas Holt
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "battery.hpp"

// Keep assertions active in the Release build used by the project checks.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <map>
#include <string>

using midnight::BatteryMonitor;
using midnight::BatteryState;

int main() {
  const auto monitor_for = [](std::map<std::string, int> values) {
    return BatteryMonitor([values = std::move(values)](const char* name) -> std::optional<int> {
      const auto it = values.find(name);
      return it == values.end() ? std::nullopt : std::optional<int>(it->second);
    });
  };

  assert(!monitor_for({}).read());
  assert(!monitor_for({{"hw.acpi.battery.units", 0}, {"hw.acpi.battery.state", 0}}).read());
  assert(!monitor_for({{"hw.acpi.battery.units", 1}, {"hw.acpi.battery.state", 7}}).read());
  assert(!monitor_for({{"hw.acpi.battery.units", -1}, {"hw.acpi.battery.state", 0}}).read());

  auto unknown = monitor_for({{"hw.acpi.battery.units", 1}, {"hw.acpi.battery.state", 0},
                              {"hw.acpi.battery.life", -1}, {"hw.acpi.battery.time", -1}}).read();
  assert(unknown);
  assert(!unknown->percent && !unknown->minutes && !unknown->on_ac && !unknown->charging);

  auto aggregate = monitor_for({{"hw.acpi.battery.units", 2}, {"hw.acpi.battery.state", 1},
                                {"hw.acpi.battery.life", 62}, {"hw.acpi.battery.time", 95},
                                {"hw.acpi.acline", 0}}).read();
  assert(aggregate);
  assert(*aggregate == (BatteryState{62, 95, false, false}));

  auto mixed = monitor_for({{"hw.acpi.battery.units", 2}, {"hw.acpi.battery.state", 3},
                            {"hw.acpi.battery.life", 70}}).read();
  assert(mixed && mixed->charging && !mixed->minutes && !mixed->on_ac);

  auto charging = monitor_for({{"hw.acpi.battery.units", 1}, {"hw.acpi.battery.state", 2},
                               {"hw.acpi.battery.life", 100}, {"hw.acpi.battery.time", -1}}).read();
  assert(charging);
  assert(charging->charging && !charging->minutes && !charging->on_ac);

  auto no_estimate = monitor_for({{"hw.acpi.battery.units", 1}, {"hw.acpi.battery.state", 1},
                                  {"hw.acpi.battery.time", 0}}).read();
  assert(no_estimate && !no_estimate->minutes);

  auto out_of_bounds = monitor_for({{"hw.acpi.battery.units", 1}, {"hw.acpi.battery.state", 0},
                                    {"hw.acpi.battery.life", 101}, {"hw.acpi.battery.time", -2},
                                    {"hw.acpi.acline", 2}}).read();
  assert(out_of_bounds && !out_of_bounds->percent && !out_of_bounds->minutes && !out_of_bounds->on_ac);
  assert(!monitor_for({{"hw.acpi.battery.units", 1}, {"hw.acpi.battery.state", 8}}).read());
}
