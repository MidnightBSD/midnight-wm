#pragma once

#include "config.hpp"
#include <cairo/cairo.h>

namespace midnight {

enum class WindowControl { minimize, maximize, close };

// Shared Warp-style vector glyphs for managed frames and built-in folders.
// Coordinates and size are in the caller's drawing coordinate system.
inline void draw_window_control(cairo_t* cr, WindowControl control,
                                double x, double y, double size,
                                const ThemeConfig& theme, bool maximized = false) {
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, size / 10.0, size / 10.0);
  cairo_set_line_width(cr, 1.0);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
  const auto symbol = [&](double offset) {
    if (control == WindowControl::minimize) {
      cairo_move_to(cr, 1 + offset, 3 + offset);
      cairo_line_to(cr, 9 + offset, 3 + offset);
      cairo_line_to(cr, 5 + offset, 7 + offset);
      cairo_close_path(cr);
    } else if (control == WindowControl::maximize) {
      if (maximized) {
        // Overlapping windows distinguish Restore from Maximize.
        cairo_rectangle(cr, 3 + offset, 1 + offset, 6, 6);
        cairo_move_to(cr, 1 + offset, 3 + offset);
        cairo_line_to(cr, 1 + offset, 9 + offset);
        cairo_line_to(cr, 7 + offset, 9 + offset);
        cairo_line_to(cr, 7 + offset, 7 + offset);
      } else {
        cairo_rectangle(cr, 1 + offset, 1 + offset, 8, 8);
      }
    } else {
      cairo_rectangle(cr, 1 + offset, 1 + offset, 8, 8);
      cairo_move_to(cr, 2 + offset, 8 + offset);
      cairo_line_to(cr, 8 + offset, 2 + offset);
    }
    cairo_stroke(cr);
  };
  const auto& highlight = theme.highlight_color;
  cairo_set_source_rgb(cr, theme.dark_mode ? highlight[0] : 1,
                      theme.dark_mode ? highlight[1] : 1,
                      theme.dark_mode ? highlight[2] : 1);
  symbol(.5);
  const auto& shadow = theme.shadow_color;
  cairo_set_source_rgb(cr, theme.dark_mode ? theme.text_color[0] : shadow[0],
                      theme.dark_mode ? theme.text_color[1] : shadow[1],
                      theme.dark_mode ? theme.text_color[2] : shadow[2]);
  symbol(-.5);
  cairo_restore(cr);
}

} // namespace midnight
