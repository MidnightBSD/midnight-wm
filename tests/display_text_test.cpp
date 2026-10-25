// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "display_text.hpp"

using midnight::is_display_unsafe;
using midnight::sanitize_display_text;
using namespace std::string_view_literals;

constexpr auto bad = "\xef\xbf\xbd"sv;  // U+FFFD
constexpr bool same(std::string_view text) { return sanitize_display_text(text) == text; }
constexpr std::string twice(std::string_view a, std::string_view b) { return std::string(a) + std::string(b); }

static_assert(same("/home/user/Documents/report.txt"));
static_assert(same("/tmp/caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80.txt"));   // é € 😀 survive
static_assert(same("\xd9\x85\xd8\xaa\xd9\x86"));                            // Arabic letters survive
static_assert(same("\xe0\xa4\x95\xe2\x80\x8d\xe0\xa4\xb7"));                 // ZWJ inside Devanagari survives
static_assert(sanitize_display_text("a\nb") == twice("a", bad) + "b");
static_assert(sanitize_display_text("a\tb\x7f") == twice("a", bad) + "b" + std::string(bad));
static_assert(sanitize_display_text("report.txt\xe2\x80\xaetxt.hs") == "report.txt" + std::string(bad) + "txt.hs");  // RLO
static_assert(sanitize_display_text("\xe2\x81\xa6x\xe2\x81\xa9") == twice(bad, "x") + std::string(bad));  // LRI/PDI
static_assert(sanitize_display_text("x\xe2\x80\xa8y") == twice("x", bad) + "y");  // line separator
static_assert(sanitize_display_text("\xef\xbb\xbfx") == twice(bad, "x"));        // BOM
static_assert(sanitize_display_text("\xc2\x85x") == twice(bad, "x"));            // C1 NEL
static_assert(sanitize_display_text("a\xffz") == twice("a", bad) + "z");         // stray byte
static_assert(sanitize_display_text("a\xc3") == twice("a", bad));                // truncated sequence
static_assert(sanitize_display_text("\xc0\xafx") == twice(bad, bad) + "x");      // overlong encoding
static_assert(sanitize_display_text("\xed\xa0\x80x") == twice(bad, bad) + std::string(bad) + "x");  // surrogate
static_assert(sanitize_display_text("") == "");
static_assert(is_display_unsafe(0x202e) && is_display_unsafe(0x2069) && is_display_unsafe(0x061c));
static_assert(!is_display_unsafe('A') && !is_display_unsafe(0x200d) && !is_display_unsafe(0x1f600));
int main() {}
