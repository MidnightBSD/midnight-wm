#pragma once

#include "config.hpp"
#include <cairo/cairo.h>

namespace midnight {

// Keep the accent strongest behind the title, fading gently toward the controls.
inline void draw_title_bar(cairo_t* cr, double x, double y, double width,
                           double height, const ThemeConfig& theme, bool active = true) {
  if (width <= 0 || height <= 0) return;
  cairo_save(cr);
  auto* gradient = cairo_pattern_create_linear(x, y, x + width, y);
  const auto& accent = theme.accent_color;
  const double r = active ? accent[0] : accent[0] * .65 + .18;
  const double g = active ? accent[1] : accent[1] * .65 + .18;
  const double b = active ? accent[2] : accent[2] * .65 + .18;
  const double fade = theme.dark_mode ? .10 : .18;
  cairo_pattern_add_color_stop_rgb(gradient, 0, r * .82, g * .82, b * .82);
  cairo_pattern_add_color_stop_rgb(gradient, .35, r, g, b);
  cairo_pattern_add_color_stop_rgb(gradient, 1,
      r * (1 - fade) + fade, g * (1 - fade) + fade, b * (1 - fade) + fade);
  cairo_rectangle(cr, x, y, width, height);
  cairo_set_source(cr, gradient);
  cairo_fill(cr);
  cairo_pattern_destroy(gradient);
  cairo_restore(cr);
}

} // namespace midnight
