// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include "display_scale.hpp"

using midnight::automatic_display_scale;
using midnight::display_dpi;
using midnight::display_scale;

// Framework 13: 2256x1504 on a 285x190 mm panel.
static_assert(display_dpi(2256, 1504, 285, 190) >= 195 && display_dpi(2256, 1504, 285, 190) <= 205);
static_assert(display_scale(1504, 0, display_dpi(2256, 1504, 285, 190)) == 1.5);
// Rotation swaps the pixel axes but not always the reported size.
static_assert(display_dpi(1504, 2256, 285, 190) == display_dpi(2256, 1504, 285, 190));
// Desktop monitors keep the height table.
static_assert(display_scale(1080, 0, display_dpi(1920, 1080, 527, 296)) == 1.0);
static_assert(display_scale(1440, 0, display_dpi(2560, 1440, 597, 336)) == 1.25);
static_assert(display_scale(1440, 0, display_dpi(3440, 1440, 800, 335)) == 1.25);
static_assert(display_scale(2160, 0, display_dpi(3840, 2160, 597, 336)) == 2.0);
// Dense laptop panels.
static_assert(display_scale(1080, 0, display_dpi(1920, 1080, 309, 174)) == 1.25); // 14 inch
static_assert(display_scale(1080, 0, display_dpi(1920, 1080, 344, 194)) == 1.0);  // 15.6 inch
static_assert(display_scale(1800, 0, display_dpi(2880, 1800, 285, 190)) == 2.0);
// Xvfb and other 96 DPI servers are unchanged.
static_assert(display_scale(720, 0, display_dpi(1280, 720, 339, 191)) == 1.0);
static_assert(display_scale(1440, 0, display_dpi(2560, 1440, 677, 381)) == 1.25);
// Missing, aspect-ratio, tiny, and absurd sizes fall back to the height.
static_assert(display_dpi(1920, 1080, 0, 0) == 0);
static_assert(display_dpi(1920, 1080, 160, 90) == 0);
static_assert(display_dpi(1920, 1080, 16, 9) == 0);
static_assert(display_dpi(1920, 1080, -5, 300) == 0);
static_assert(display_dpi(65535, 65535, 101, 1) == 0);
static_assert(display_dpi(100, 100, 30000, 30000) == 0);
static_assert(automatic_display_scale(1504) == 1.25);
// An explicit setting always wins; out-of-range values are ignored.
static_assert(display_scale(1504, 1.75, 200) == 1.75);
static_assert(display_scale(1504, 5.0, 200) == 1.5);

int main() { return 0; }
