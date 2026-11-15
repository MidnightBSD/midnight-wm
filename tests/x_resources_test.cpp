// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include "x_resources.hpp"

#include <iostream>

using midnight::may_manage_xft_dpi;
using midnight::with_xft_dpi;
using midnight::xft_dpi_resource;

static_assert(!xft_dpi_resource(""));
static_assert(xft_dpi_resource("Xft.dpi:\t144\n") == 144);
static_assert(xft_dpi_resource("XTerm*faceName: Mono\n  Xft.dpi :  120 \r\nother: 1") == 120);
static_assert(xft_dpi_resource("Xft.dpi: 96\nXft.dpi: 110\n") == 110);
static_assert(!xft_dpi_resource("Xft.dpi: abc\n"));
static_assert(!xft_dpi_resource("Xft.dpi: -5\n"));
static_assert(!xft_dpi_resource("Xft.dpi: 99999999999999\n"));
static_assert(!xft_dpi_resource("Xft.dpiX: 144\nXft.antialias: 1\n! Xft.dpi 144\n"));
static_assert(may_manage_xft_dpi({}, {}));
static_assert(may_manage_xft_dpi(96, {}));
static_assert(may_manage_xft_dpi(144, 144));
static_assert(!may_manage_xft_dpi(120, {}));
static_assert(!may_manage_xft_dpi(120, 144));

int main() {
  bool passed = true;
  const auto check = [&](const std::string& actual, const std::string& expected, const char* name) {
    if (actual == expected) return;
    std::cerr << name << ": [" << actual << "] instead of [" << expected << "]\n";
    passed = false;
  };
  check(with_xft_dpi("", 144), "Xft.dpi:\t144\n", "empty database");
  check(with_xft_dpi("XTerm*faceName: Mono\nXft.dpi: 96\n*color0: #000\n", 144),
        "XTerm*faceName: Mono\n*color0: #000\nXft.dpi:\t144\n", "replaces and keeps others");
  check(with_xft_dpi("a: 1", 120), "a: 1\nXft.dpi:\t120\n", "missing final newline");
  check(with_xft_dpi("Xft.dpi: 1\na: 1\nXft.dpi: 2\n", {}), "a: 1\n", "removes every entry");
  check(with_xft_dpi("a: 1\n\nb: 2\n", {}), "a: 1\n\nb: 2\n", "unrelated text is unchanged");
  return passed ? 0 : 1;
}
