// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/sysctl.h>

namespace {

using SysctlByName = int (*)(const char*, void*, std::size_t*, const void*, std::size_t);

struct Fixture {
  int units;
  int life;
  int time;
  int state;
  int acline;
};

bool read_fixture(Fixture& fixture) {
  const char* path = std::getenv("MIDNIGHT_TEST_BATTERY_FILE");
  if (path == nullptr) {
    errno = ENOENT;
    return false;
  }
  FILE* file = std::fopen(path, "r");
  if (file == nullptr) return false;
  const int count = std::fscanf(file, "%d %d %d %d %d", &fixture.units, &fixture.life,
                                &fixture.time, &fixture.state, &fixture.acline);
  int extra = std::fgetc(file);
  while (extra != EOF && std::isspace(static_cast<unsigned char>(extra))) extra = std::fgetc(file);
  const int close_result = std::fclose(file);
  if (count != 5 || extra != EOF || close_result != 0) {
    errno = EINVAL;
    return false;
  }
  return true;
}

int fixture_value(const char* name, const Fixture& fixture, int& value) {
  if (std::strcmp(name, "hw.acpi.battery.units") == 0) value = fixture.units;
  else if (std::strcmp(name, "hw.acpi.battery.life") == 0) value = fixture.life;
  else if (std::strcmp(name, "hw.acpi.battery.time") == 0) value = fixture.time;
  else if (std::strcmp(name, "hw.acpi.battery.state") == 0) value = fixture.state;
  else if (std::strcmp(name, "hw.acpi.acline") == 0) value = fixture.acline;
  else return 0;
  return 1;
}

}  // namespace

extern "C" int sysctlbyname(const char* name, void* old_value, std::size_t* old_length,
                            const void* new_value, std::size_t new_length) {
  if (name == nullptr) {
    errno = EFAULT;
    return -1;
  }
  Fixture fixture{};
  int value = 0;
  if (fixture_value(name, fixture, value)) {
    if (!read_fixture(fixture)) return -1;
    fixture_value(name, fixture, value);
    if (new_value != nullptr || new_length != 0) {
      errno = EPERM;
      return -1;
    }
    if (old_length == nullptr) {
      errno = EFAULT;
      return -1;
    }
    if (old_value == nullptr) {
      *old_length = sizeof(value);
      return 0;
    }
    if (*old_length < sizeof(value)) {
      *old_length = sizeof(value);
      errno = ENOMEM;
      return -1;
    }
    std::memcpy(old_value, &value, sizeof(value));
    *old_length = sizeof(value);
    return 0;
  }
  const auto next = reinterpret_cast<SysctlByName>(dlsym(RTLD_NEXT, "sysctlbyname"));
  if (next == nullptr) {
    errno = ENOSYS;
    return -1;
  }
  return next(name, old_value, old_length, new_value, new_length);
}
