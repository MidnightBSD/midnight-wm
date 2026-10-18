// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "compositor.hpp"
#include <glib.h>
#include <xcb/xcb.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {
xcb_atom_t selection(xcb_connection_t* c) {
  constexpr char name[] = "_NET_WM_CM_S0";
  auto* r = xcb_intern_atom_reply(c, xcb_intern_atom(c, false, sizeof(name) - 1, name), nullptr);
  const auto atom = r ? r->atom : xcb_atom_t{};
  std::free(r);
  return atom;
}
xcb_window_t owner(xcb_connection_t* c, xcb_atom_t atom) {
  auto* r = xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, atom), nullptr);
  const auto window = r ? r->owner : xcb_window_t{};
  std::free(r);
  return window;
}
xcb_window_t claim(xcb_connection_t* c, xcb_atom_t atom) {
  auto* screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
  const auto window = xcb_generate_id(c);
  xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 1, 1, 0,
                    XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
  xcb_set_selection_owner(c, window, atom, XCB_CURRENT_TIME);
  xcb_flush(c);
  return window;
}
template<class Predicate> bool wait_for(Predicate predicate, midnight::Compositor* compositor = nullptr) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  do {
    while (g_main_context_iteration(nullptr, false)) {}
    if (compositor) compositor->tick();
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}
}

int main(int argc, char** argv) {
  auto* c = xcb_connect(nullptr, nullptr);
  if (xcb_connection_has_error(c)) return 1;
  const auto atom = selection(c);
  // This same binary acts as the foreground Picom fixture, without a shell.
  if (argc > 1) {
    if (std::string(argv[1]) == "--owner") {
      std::cout << owner(c, atom) << '\n';
      xcb_disconnect(c);
      return 0;
    }
    (void)claim(c, atom);
    while (!xcb_connection_has_error(c)) {
      while (auto* event = xcb_poll_for_event(c)) std::free(event);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    xcb_disconnect(c);
    return 0;
  }
  bool passed = true;
  const auto check = [&](bool ok, const char* message) {
    if (!ok) { passed = false; std::cerr << message << '\n'; }
  };
  char* tmp = g_dir_make_tmp("midnight-compositor-test-XXXXXX", nullptr);
  if (!tmp) return 1;
  const std::filesystem::path directory(tmp);
  g_free(tmp);
  std::filesystem::create_symlink(std::filesystem::canonical(argv[0]), directory / "picom");
  const std::string original_path = g_getenv("PATH") ? g_getenv("PATH") : "";
  g_setenv("PATH", directory.c_str(), true);
  const auto external = claim(c, atom);
  {
    midnight::Compositor compositor(c, 0);
    compositor.set_enabled(true);
    compositor.tick();
    check(owner(c, atom) == external, "existing compositor was replaced");
    compositor.set_enabled(false);
  }
  check(owner(c, atom) == external, "external compositor stopped at shutdown");
  xcb_destroy_window(c, external);
  xcb_flush(c);
  {
    midnight::Compositor compositor(c, 0);
    compositor.set_enabled(true);
    check(wait_for([&] { return owner(c, atom) != XCB_NONE; }, &compositor), "owned compositor did not start");
    compositor.set_enabled(false);
    check(wait_for([&] { return owner(c, atom) == XCB_NONE; }, &compositor), "disabled compositor kept selection");
    compositor.set_enabled(true);
    check(wait_for([&] { return owner(c, atom) != XCB_NONE; }, &compositor), "live re-enable did not restart compositor");
  }
  check(wait_for([&] { return owner(c, atom) == XCB_NONE; }), "owned compositor survived session shutdown");
  g_setenv("PATH", "/nonexistent", true);
  {
    midnight::Compositor compositor(c, 0);
    compositor.set_enabled(true);
    compositor.tick();
    check(owner(c, atom) == XCB_NONE, "missing Picom must leave display uncomposited");
    g_setenv("PATH", directory.c_str(), true);
    compositor.set_enabled(false);
    compositor.set_enabled(true);
    check(wait_for([&] { return owner(c, atom) != XCB_NONE; }, &compositor), "retry after missing dependency failed");
  }
  check(wait_for([&] { return owner(c, atom) == XCB_NONE; }), "retry child survived shutdown");
  g_setenv("PATH", original_path.c_str(), true);
  std::filesystem::remove_all(directory);
  xcb_disconnect(c);
  return passed ? 0 : 1;
}
