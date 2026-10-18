// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "task_icon.hpp"
#include <iostream>
#include <vector>

int main() {
  bool passed = true;
  const auto check = [&](bool value, const char* message) {
    if (!value) { passed = false; std::cerr << message << '\n'; }
  };
  check(!midnight::decode_task_icon({}), "empty icon accepted");
  for (const auto& data : std::vector<std::vector<std::uint32_t>>{
      {1}, {0, 1, 0}, {1, 0}, {10, 10, 0}, {0xffffffff, 0xffffffff, 0}, {4097, 1, 0}})
    check(!midnight::decode_task_icon(data), "malformed icon accepted");
  const std::vector<std::uint32_t> transparent{1, 1, 0x80ff8040};
  auto icon = midnight::decode_task_icon(transparent);
  check(bool(icon), "valid pixel rejected");
  if (icon) {
    check(*reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(icon.get())) == 0x80804020,
          "ARGB was not premultiplied correctly");
  }
  std::vector<std::uint32_t> choices{1, 1, 0xff0000ff, 64, 64};
  choices.insert(choices.end(), 64 * 64, 0xffff0000);
  choices.insert(choices.end(), {256, 256});
  choices.insert(choices.end(), 256 * 256, 0xff00ff00);
  choices.push_back(0xffffffff); // A truncated tail must not discard preceding valid icons.
  icon = midnight::decode_task_icon(choices);
  check(icon && cairo_image_surface_get_width(icon.get()) == 64, "incorrect icon size selected");
  if (icon) check(*reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(icon.get())) == 0xffff0000,
                  "incorrect icon variant selected");
  std::vector<std::uint32_t> large{512, 256};
  large.insert(large.end(), 512 * 256, 0xff804020);
  icon = midnight::decode_task_icon(large);
  check(icon && cairo_image_surface_get_width(icon.get()) == 64 &&
        cairo_image_surface_get_height(icon.get()) == 32, "large icon not bounded or aspect ratio lost");
  std::vector<std::uint32_t> too_large(1024 * 1024 + 1);
  check(!midnight::decode_task_icon(too_large), "property limit not enforced");
  return passed ? 0 : 1;
}
