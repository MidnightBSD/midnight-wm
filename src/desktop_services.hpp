// Copyright (c) 2026 Lucas Holt. All rights reserved.
#pragma once
#include "config.hpp"
#include <cstddef>
#include <memory>
#include <xcb/xcb.h>

namespace midnight {
// Optional session services. Dispatch the default GLib context and tick from
// the main loop. Neither service replaces an existing bus name owner.
class DesktopServices {
 public:
  DesktopServices(xcb_connection_t* connection, const ThemeConfig& theme);
  ~DesktopServices();
  DesktopServices(const DesktopServices&) = delete;
  DesktopServices& operator=(const DesktopServices&) = delete;
  bool initialize();
  void refresh(const ThemeConfig& theme);
  void tick();
  [[nodiscard]] std::size_t inhibitor_count() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
