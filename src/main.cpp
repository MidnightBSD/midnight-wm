#include "autostart.hpp"
#include "clipboard.hpp"
#include "dnd.hpp"
#include "file_manager1.hpp"
#include "keyring_session.hpp"
#include "dbus_menu.hpp"
#include "dbus_menu_popup.hpp"
#include "notifications.hpp"
#include "shell.hpp"
#include "startup_notification.hpp"
#include "status_notifier.hpp"
#include "status_panel.hpp"
#include "store.hpp"
#include "tray.hpp"
#include "x11.hpp"
#include "xsettings.hpp"

#include <gio/gio.h>
#include <glib.h>
#include <xcb/xcb.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <thread>
#include <string_view>
#include <unistd.h>

namespace {
std::atomic_bool running{true};
void stop(int) { running = false; }

bool can_connect_to_bus(const char* addresses) {
  if (!addresses || !*addresses) return false;
  char** parts = g_strsplit(addresses, ",", -1);
  bool connected = false;
  for (char** part = parts; part && *part; ++part) {
    // The autolaunch transport can create a bus during this check. Let the
    // managed dbus-run-session wrapper do that instead.
    if (!**part || g_str_has_prefix(*part, "autolaunch:")) continue;
    GError* error = nullptr;
    auto* bus = g_dbus_connection_new_for_address_sync(
        *part, static_cast<GDBusConnectionFlags>(
                   G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    if (error) g_error_free(error);
    if (!bus) continue;
    g_dbus_connection_set_exit_on_close(bus, FALSE);
    connected = true;
    (void)g_dbus_connection_close_sync(bus, nullptr, nullptr);
    g_object_unref(bus);
    break;
  }
  g_strfreev(parts);
  return connected;
}

void ensure_session_bus(char* program) {
  if (const char* address = g_getenv("DBUS_SESSION_BUS_ADDRESS"); can_connect_to_bus(address)) {
    g_unsetenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED");
    return;
  }

  // A display manager can provide a bus at the standard runtime path without
  // exporting its address. Give GIO and launched apps the same address.
  if (const char* runtime = g_getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
    char* path = g_build_filename(runtime, "bus", nullptr);
    char* escaped = g_dbus_address_escape_value(path);
    const std::string address = std::string("unix:path=") + escaped;
    g_free(escaped);
    g_free(path);
    if (can_connect_to_bus(address.c_str())) {
      g_setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), TRUE);
      g_unsetenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED");
      return;
    }
  }

  // The child inherits this marker so a failed wrapper cannot recurse.
  if (g_getenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED")) {
    g_unsetenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED");
    std::cerr << "D-Bus session unavailable after startup\n";
    return;
  }
  char* wrapper = g_find_program_in_path("dbus-run-session");
  char* daemon = g_find_program_in_path("dbus-daemon");
  if (!wrapper || !daemon) {
    std::cerr << "D-Bus session unavailable (dbus-run-session or dbus-daemon is missing)\n";
    g_free(wrapper);
    g_free(daemon);
    return;
  }
  g_free(daemon);
  g_setenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED", "1", TRUE);
  char separator[] = "--";
  char* arguments[]{wrapper, separator, program, nullptr};
  execv(wrapper, arguments);
  const int failure = errno;
  g_unsetenv("MIDNIGHT_WM_DBUS_BOOTSTRAPPED");
  std::cerr << "Could not start D-Bus session: " << std::strerror(failure) << '\n';
  g_free(wrapper);
}
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

  ensure_session_bus(argv[0]);

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

  midnight::KeyringSession keyring;
  keyring.initialize();

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
      midnight::StartupNotification startup(connection, screen, screen_number);
      wm.set_startup_notification(&startup);
      shell.set_startup_notification(&startup);
      midnight::StatusNotifier notifier;
      midnight::DbusMenu dbus_menu;
      midnight::X11SystemTray tray(connection, screen, screen_number, shell);
      midnight::DbusMenuPopup menu_popup(connection, screen, shell, dbus_menu);
      midnight::StatusNotifierPanel status_panel(connection, screen, shell, tray, notifier, menu_popup);
      midnight::Notifications notifications(connection, screen, shell);
      midnight::FileManager1 file_manager(
          [&shell](const std::filesystem::path& path,
                   const std::optional<std::filesystem::path>& selected) {
            return shell.show_folder(path, selected);
          },
          [&shell](const std::filesystem::path& path) {
            return shell.show_item_properties(path);
          });
      midnight::XSettings xsettings(connection, screen, screen_number);
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
        // Existing notification daemons retain the standard session name.
        (void)notifications.initialize();
        if (!file_manager.initialize()) std::cerr << "FileManager1 D-Bus unavailable\n";
        (void)xsettings.initialize();
        midnight::launch_autostart();

        pollfd display{.fd = xcb_get_file_descriptor(connection), .events = POLLIN, .revents = 0};
        while (running && !xcb_connection_has_error(connection)) {
          while (auto* event = xcb_poll_for_event(connection)) {
            if (!clipboard.handle(event) && !dnd.handle(event) && !startup.handle(event) &&
                !xsettings.handle(event) && !notifications.handle(event) &&
                !menu_popup.handle(event) &&
                !shell.handle(event) &&
                !tray.handle(event) && !status_panel.handle(event)) wm.handle(event);
            std::free(event);
          }
          while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
          shell.tick();
          tray.tick();
          status_panel.tick();
          dnd.tick();
          notifications.tick();
          startup.tick();
          xsettings.tick();
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
