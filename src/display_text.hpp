// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace midnight {

// True for code points that must not reach a single-line label: C0/C1 controls,
// line separators, and Unicode formatting characters that can reorder or hide
// text so a path appears to name a different file than the one it opens.
constexpr bool is_display_unsafe(std::uint32_t cp) {
  if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f)) return true;  // C0, DEL, C1
  if (cp == 0x061c) return true;                              // Arabic letter mark
  if (cp == 0x200b || cp == 0x200e || cp == 0x200f) return true;  // ZWSP, LRM, RLM
  if (cp >= 0x202a && cp <= 0x202e) return true;              // LRE..RLO bidi embeddings/overrides
  if (cp == 0x2028 || cp == 0x2029) return true;              // line/paragraph separators
  if (cp == 0x2060) return true;                              // word joiner
  if (cp >= 0x2066 && cp <= 0x2069) return true;              // bidi isolates
  if (cp == 0xfeff) return true;                              // byte order mark
  return false;
}

// Returns a copy of the UTF-8 text with unsafe code points and malformed
// sequences replaced by U+FFFD so the visible text cannot misrepresent itself.
constexpr std::string sanitize_display_text(std::string_view text) {
  constexpr std::string_view replacement = "\xef\xbf\xbd";
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    std::size_t length = 0;
    std::uint32_t cp = 0;
    if (lead < 0x80) { length = 1; cp = lead; }
    else if ((lead & 0xe0) == 0xc0) { length = 2; cp = lead & 0x1f; }
    else if ((lead & 0xf0) == 0xe0) { length = 3; cp = lead & 0x0f; }
    else if ((lead & 0xf8) == 0xf0) { length = 4; cp = lead & 0x07; }
    bool valid = length != 0 && i + length <= text.size();
    for (std::size_t k = 1; valid && k < length; ++k) {
      const auto byte = static_cast<unsigned char>(text[i + k]);
      if ((byte & 0xc0) != 0x80) valid = false;
      else cp = (cp << 6) | (byte & 0x3f);
    }
    if (valid) {
      // Reject overlong forms, surrogates, and values past U+10FFFF.
      if ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000) ||
          (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) valid = false;
    }
    if (!valid) {
      out += replacement;
      ++i;
      continue;
    }
    if (is_display_unsafe(cp)) out += replacement;
    else out.append(text.substr(i, length));
    i += length;
  }
  return out;
}

}  // namespace midnight
