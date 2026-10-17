#pragma once

#include "config.hpp"
#include <pango/pango.h>
#include <algorithm>
#include <cmath>

namespace midnight {

// Font sizes in config use Pango points; omitted sizes preserve each widget's
// existing size. The caller owns the returned description.
inline PangoFontDescription* make_ui_font(const ThemeConfig& theme, bool title,
                                          double default_size, bool absolute,
                                          double scale = 1.0) {
  auto* font = pango_font_description_from_string(
      (title ? theme.title_font : theme.ui_font).c_str());
  double size = default_size;
  if (pango_font_description_get_set_fields(font) & PANGO_FONT_MASK_SIZE) {
    size = std::clamp(pango_font_description_get_size(font) / double(PANGO_SCALE), 6.0, 32.0);
    const bool configured_absolute = pango_font_description_get_size_is_absolute(font);
    if (absolute && !configured_absolute) size *= 4.0 / 3.0;
    else if (!absolute && configured_absolute) size *= 3.0 / 4.0;
  }
  if (absolute) pango_font_description_set_absolute_size(font, size * scale * PANGO_SCALE);
  else pango_font_description_set_size(font, static_cast<int>(std::lround(size * scale * PANGO_SCALE)));
  return font;
}

} // namespace midnight
