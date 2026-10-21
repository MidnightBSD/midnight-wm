// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include "config.hpp"
#include "shortcuts.hpp"

#include <X11/keysym.h>

#include <iostream>

int main() {
  bool passed = true;
  const auto check = [&](bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; passed = false; }
  };
  const auto defaults = midnight::default_shortcuts();
  for (const auto& chord : defaults) {
    check(chord.has_value(), "default shortcut missing");
    if (chord) {
      const auto parsed = midnight::parse_shortcut(midnight::format_shortcut(chord));
      check(parsed && *parsed == chord, "default shortcut did not round trip");
    }
  }
  check(!midnight::shortcuts_conflict(defaults), "default shortcuts conflict");
  const auto alias = midnight::parse_shortcut(" Control + Alt + LEFT ");
  check(alias && *alias == defaults[0], "modifier/key aliases failed");
  const auto function = midnight::parse_shortcut("Super+Shift+F12");
  check(function && *function == std::optional<midnight::KeyChord>{{XK_F12,
        XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT}}, "shifted function shortcut failed");
  const auto disabled = midnight::parse_shortcut("none");
  check(disabled && !*disabled, "None did not disable");
  for (const auto* text : {"A", "Shift+A", "Ctrl+", "Ctrl+Ctrl+A", "Alt+WrongKey", "Ctrl+Alt+A+B"})
    check(!midnight::parse_shortcut(text), "invalid shortcut was accepted");
  check(midnight::shortcut_action(defaults, XK_Left, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 |
        XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2) == midnight::ShortcutAction::workspace_left,
        "lock keys changed shortcut matching");
  check(!midnight::shortcut_action(defaults, XK_Left, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 |
        XCB_MOD_MASK_SHIFT), "unconfigured Shift variant matched");
  auto custom = midnight::parse_config("[Shortcuts]\nworkspace_left=Super+F8\nclose_window=None\n");
  check(custom && custom->shortcuts[0] == midnight::parse_shortcut("Super+F8").value() &&
        !custom->shortcuts[6], "custom or disabled shortcut did not load");
  if (custom) {
    check(midnight::shortcut_action(custom->shortcuts, XK_F8, XCB_MOD_MASK_4) ==
          midnight::ShortcutAction::workspace_left, "custom shortcut did not dispatch");
    check(!midnight::shortcut_action(custom->shortcuts, XK_Left,
          XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1), "replaced default shortcut still active");
  }
  const auto conflict = midnight::parse_config("[Shortcuts]\nworkspace_left=Alt+Tab\n");
  check(conflict && conflict->shortcuts == defaults, "conflicting config did not retain defaults");
  const auto invalid = midnight::parse_config("[Shortcuts]\nworkspace_left=BadKey\n");
  check(invalid && invalid->shortcuts == defaults, "invalid config did not retain defaults");
  return passed ? 0 : 1;
}
