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

#include <utility>

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

namespace midnight {
namespace {
constexpr int kAcpiNotPresent = 0x7;
constexpr int kDischarging = 0x1;
constexpr int kCharging = 0x2;
constexpr int kCritical = 0x4;

std::optional<int> native_read_integer(const char* name) {
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
  int value = 0;
  std::size_t size = sizeof(value);
  if (sysctlbyname(name, &value, &size, nullptr, 0) != 0 || size != sizeof(value))
    return std::nullopt;
  return value;
#else
  (void)name;
  return std::nullopt;
#endif
}

bool valid_state(int state) {
  return state >= 0 && (state & ~(kDischarging | kCharging | kCritical)) == 0;
}
} // namespace

BatteryMonitor::BatteryMonitor() : reader_(native_read_integer) {}

BatteryMonitor::BatteryMonitor(ReadInteger reader) : reader_(std::move(reader)) {}

std::optional<BatteryState> BatteryMonitor::read() const {
  if (!reader_) return std::nullopt;

  const auto units = reader_("hw.acpi.battery.units");
  const auto state = reader_("hw.acpi.battery.state");
  if (!units || !state || *units < 0 || !valid_state(*state) || *units == 0 ||
      *state == kAcpiNotPresent)
    return std::nullopt;

  BatteryState result;
  if (const auto percent = reader_("hw.acpi.battery.life")) {
    if (*percent >= 0 && *percent <= 100) {
      result.percent = *percent;
    }
  }
  if (const auto minutes = reader_("hw.acpi.battery.time")) {
    if (*minutes > 0) {
      result.minutes = *minutes;
    }
  }
  if (const auto ac_line = reader_("hw.acpi.acline")) {
    if (*ac_line == 0 || *ac_line == 1)
      result.on_ac = (*ac_line == 1);
  }
  result.charging = (*state & kCharging) != 0;
  return result;
}
} // namespace midnight
