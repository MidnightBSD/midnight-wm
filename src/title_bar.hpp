#pragma once

#include "config.hpp"
#include <cairo/cairo.h>

namespace midnight {

// Draw two nested, pixel-aligned outlines inside the existing frame extents.
// Callers use device pixels for WM frames and logical pixels for shell windows.
inline void draw_window_outline(cairo_t* cr, double width, double height,
                                double line_width, const ThemeConfig& theme) {
  if (theme.window_border_style != "two-color" || line_width <= 0 ||
      width < 4 * line_width || height < 4 * line_width) return;
  cairo_save(cr);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  double inset = 0;
  for (const auto& color : {theme.window_border_outer_color, theme.window_border_inner_color}) {
    cairo_set_source_rgb(cr, color[0], color[1], color[2]);
    cairo_rectangle(cr, inset, inset, width - 2 * inset, height - 2 * inset);
    cairo_rectangle(cr, inset + line_width, inset + line_width,
                    width - 2 * (inset + line_width), height - 2 * (inset + line_width));
    cairo_fill(cr);
    inset += line_width;
  }
  cairo_restore(cr);
}

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
