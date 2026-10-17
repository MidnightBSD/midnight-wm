#include "autostart.hpp"
#include "clipboard.hpp"
#include "dnd.hpp"
#include "dbus_menu.hpp"
#include "dbus_menu_popup.hpp"
#include "shell.hpp"
#include "status_notifier.hpp"
#include "status_panel.hpp"
#include "store.hpp"
#include "tray.hpp"
#include "x11.hpp"

#include <glib.h>
#include <xcb/xcb.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <thread>
#include <string_view>

namespace {
std::atomic_bool running{true};
void stop(int) { running = false; }
}

int main(int argc, char** argv) {
  if (argc > 1) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
      std::cout << "midnight-wm " MIDNIGHT_WM_VERSION "\n";
      return 0;
    }
    std::cerr << "Usage: midnight-wm [--version]\n";
    return 2;
  }

  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);

  // Display managers may supply multiple desktop names. Keep that value when
  // present, and identify direct .xinitrc sessions for desktop entry filters.
  if (!std::getenv("XDG_CURRENT_DESKTOP") || !*std::getenv("XDG_CURRENT_DESKTOP"))
    g_setenv("XDG_CURRENT_DESKTOP", "MidnightWorkspace", true);

  int screen_number = 0;
  xcb_connection_t* connection = xcb_connect(nullptr, &screen_number);
  if (!connection || xcb_connection_has_error(connection)) {
    std::cerr << "Could not connect to the X server\n";
    if (connection) xcb_disconnect(connection);
    return 1;
  }
  auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
  for (int i = 0; i < screen_number; ++i) xcb_screen_next(&screens);
  auto* screen = screens.data;
  if (!screen) {
    std::cerr << "X screen unavailable\n";
    xcb_disconnect(connection);
    return 1;
  }

  int result = 0;
  {
    midnight::Store store(midnight::Store::data_path() / "workspace.db");
    if (auto opened = store.open(); !opened) {
      std::cerr << "Workspace database: " << opened.error() << '\n';
      result = 1;
    } else {
      midnight::X11Dnd dnd(connection, screen->root);
      midnight::Shell shell(connection, screen, store);
      midnight::X11Clipboard clipboard(connection, screen->root);
      midnight::X11WindowManager wm(connection, screen, shell);
      midnight::StatusNotifier notifier;
      midnight::DbusMenu dbus_menu;
      midnight::X11SystemTray tray(connection, screen, screen_number, shell);
      midnight::DbusMenuPopup menu_popup(connection, screen, shell, dbus_menu);
      midnight::StatusNotifierPanel status_panel(connection, screen, shell, tray, notifier, menu_popup);
      shell.set_exit_handler([] { running = false; });
      if (!dnd.initialize()) std::cerr << "XDND integration unavailable\n";
      if (!clipboard.initialize()) std::cerr << "XFixes clipboard integration unavailable\n";
      shell.set_file_clipboard_handlers(
          [&clipboard](const std::vector<std::filesystem::path>& paths, bool cut) {
            clipboard.set_files(paths, cut);
          },
          [&clipboard](std::function<void(std::vector<std::filesystem::path>, bool)> callback) {
            clipboard.request_files(std::move(callback));
          });
      shell.set_dnd_handlers(
          [&dnd](xcb_window_t window, midnight::Shell::DropHandler callback) {
            dnd.register_target(window, std::move(callback));
          },
          [&dnd](xcb_window_t window) { dnd.unregister_target(window); },
          [&dnd](xcb_window_t window, std::vector<std::filesystem::path> paths, bool move) {
            (void)dnd.begin_drag(window, std::move(paths), move);
          });

      if (!wm.initialize() || !shell.initialize()) result = 1;
      else {
        if (!tray.initialize()) std::cerr << "X11 system tray unavailable\n";
        if (!status_panel.initialize()) std::cerr << "Status notifier panel unavailable\n";
        if (!notifier.initialize()) std::cerr << "Status notifier D-Bus unavailable\n";
        midnight::launch_autostart();

        pollfd display{.fd = xcb_get_file_descriptor(connection), .events = POLLIN, .revents = 0};
        while (running && !xcb_connection_has_error(connection)) {
          while (auto* event = xcb_poll_for_event(connection)) {
            if (!clipboard.handle(event) && !dnd.handle(event) && !menu_popup.handle(event) &&
                !shell.handle(event) &&
                !tray.handle(event) && !status_panel.handle(event)) wm.handle(event);
            std::free(event);
          }
          while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
          shell.tick();
          tray.tick();
          status_panel.tick();
          dnd.tick();
          xcb_flush(connection);
          if (poll(&display, 1, 100) < 0 && errno != EINTR) break;
        }
        if (xcb_connection_has_error(connection)) result = 1;
      }
    }
  }
  xcb_disconnect(connection);
  return result;
}
