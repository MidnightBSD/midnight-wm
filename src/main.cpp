#include "autostart.hpp"
#include "application_restore.hpp"
#include "clipboard.hpp"
#include "compositor.hpp"
#include "config.hpp"
#include "dnd.hpp"
#include "file_manager1.hpp"
#include "keyring_session.hpp"
#include "desktop_services.hpp"
#include "dbus_menu.hpp"
#include "dbus_menu_popup.hpp"
#include "notifications.hpp"
#include "shell.hpp"
#include "session.hpp"
#include "session_windows.hpp"
#include "session_bus.hpp"
#include "session_reaper.hpp"
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
#include <xcb/randr.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <optional>
#include <poll.h>
#include <string>
#include <thread>
#include <string_view>
#include <utility>
#include <unistd.h>

namespace {
#ifndef MIDNIGHT_WM_SETTINGS_PROGRAM
#define MIDNIGHT_WM_SETTINGS_PROGRAM "/usr/local/bin/midnightbsd-settings"
#endif

std::string settings_program(const char* program) {
  char* found = g_find_program_in_path(program);
  std::error_code error;
  const auto executable = std::filesystem::canonical(found ? found : program, error);
  g_free(found);
  if (!error) {
    const auto sibling = executable.parent_path() / "midnightbsd-settings";
    if (access(sibling.c_str(), X_OK) == 0) return sibling.string();
  }
  if (access(MIDNIGHT_WM_SETTINGS_PROGRAM, X_OK) == 0) return MIDNIGHT_WM_SETTINGS_PROGRAM;
  return "midnightbsd-settings";
}

// Poll the file itself: directory watches can miss its initial creation or an
// atomic replacement. Compare bounded snapshots, and wait for a stable read so
// ordinary editors do not apply a transient truncated file.
class ConfigReload {
 public:
  ConfigReload() : applied_(read()), pending_(applied_) {}
  std::optional<midnight::ThemeConfig> poll() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_poll_ < std::chrono::seconds(1)) return std::nullopt;
    last_poll_ = now;
    const auto contents = read();
    if (!contents) { pending_.reset(); return std::nullopt; }
    if (contents == applied_) return std::nullopt;
    if (contents != pending_) { pending_ = contents; return std::nullopt; }
    auto config = midnight::parse_config(*contents);
    // Remember rejected input too, rather than retrying the same broken file.
    applied_ = contents;
    if (!config) std::cerr << "Ignoring malformed workspace configuration\n";
    return config;
  }
 private:
  std::optional<std::string> read() const {
    const auto path = std::filesystem::path(g_get_user_config_dir()) / "midnight-wm/config.ini";
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    constexpr std::size_t limit = 1024 * 1024;
    std::string contents(limit + 1, '\0');
    input.read(contents.data(), contents.size());
    const auto length = input.gcount();
    if (length > static_cast<std::streamsize>(limit) || input.bad()) return std::nullopt;
    contents.resize(static_cast<std::size_t>(length));
    // Empty files are commonly an editor's intermediate save; retain the
    // running configuration until a complete key file is available.
    if (contents.empty()) return std::nullopt;
    return contents;
  }
  std::optional<std::string> applied_, pending_;
  std::chrono::steady_clock::time_point last_poll_{};
};

std::atomic_bool running{true};
void stop(int) { running = false; }

}

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
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

  midnight::ensure_session_bus(argc, argv);
  // After the bus bootstrap, so the dbus-run-session re-exec is not a reaper.
  // Declared before everything that launches processes so it is destroyed last.
  midnight::SessionReaper reaper;
  midnight::SessionWindowReporter window_reporter;
  auto application_plan = midnight::take_application_restore_plan(midnight::load_config().restore_applications);

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
      shell.set_settings_program(settings_program(argv[0]));
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
      midnight::FileManager1 workspace_file_manager(
          [&shell](const std::filesystem::path& path,
                   const std::optional<std::filesystem::path>& selected) {
            return shell.show_folder(path, selected);
          },
          [&shell](const std::filesystem::path& path) {
            return shell.show_item_properties(path);
          }, midnight::workspace_file_manager_name, midnight::workspace_file_manager_path);
      midnight::XSettings xsettings(connection, screen, screen_number);
      midnight::DesktopServices desktop_services(connection, shell.theme());
      shell.set_exit_handler([&] {
        if (!window_reporter.request_logout()) running = false;
        else shell.show_session_notice("Waiting for applications to save before logout.");
      });
      if (!dnd.initialize()) std::cerr << "XDND integration unavailable\n";
      if (!clipboard.initialize()) std::cerr << "XFixes clipboard integration unavailable\n";
      shell.set_file_clipboard_handlers(
          [&clipboard](const std::vector<std::filesystem::path>& paths, bool cut) {
            clipboard.set_files(paths, cut);
          },
          [&clipboard](std::function<void(std::vector<std::filesystem::path>, bool)> callback) {
            clipboard.request_files(std::move(callback));
          });
      dnd.set_drag_feedback([&shell](const std::filesystem::path& path, bool move, int x, int y) {
        shell.update_drag_feedback(path, move, x, y);
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
        if (!workspace_file_manager.initialize()) std::cerr << "Workspace file manager D-Bus unavailable\n";
        xsettings.set_dark_mode(shell.theme().dark_mode);
        (void)xsettings.initialize();
        (void)desktop_services.initialize();
        midnight::Compositor compositor(connection, screen_number);
        compositor.set_enabled(shell.theme().compositing);
        compositor.tick();
        midnight::SessionStore session;
        if (shell.theme().restore_session || shell.theme().save_session) {
          if (auto saved = session.load(); !saved) {
            std::cerr << "Ignoring saved folder session: " << saved.error() << '\n';
          } else if (*saved && shell.theme().restore_session) {
            wm.set_current_desktop((**saved).workspace);
            shell.restore_session(**saved);
          }
        }
        window_reporter.ready();
        midnight::ApplicationRestorer applications;
        if (shell.theme().restore_applications) {
          if (!application_plan) std::cerr << "Application restore skipped: " << application_plan.error() << '\n';
          else if (*application_plan) {
            wm.restore_application_windows((**application_plan).windows);
            for (const auto& error : applications.restore(**application_plan, shell.theme().restore_desktop_applications)) std::cerr << error << '\n';
          }
        }
        midnight::launch_autostart(applications.launched_executables(), applications.launched_desktop_commands());
        const bool start_xscreensaver = shell.theme().start_xscreensaver;
        const auto screensaver_check = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool screensaver_checked = false;
        ConfigReload config_reload;

        pollfd display{.fd = xcb_get_file_descriptor(connection), .events = POLLIN, .revents = 0};
        while (running && !xcb_connection_has_error(connection)) {
          while (auto* event = xcb_poll_for_event(connection)) {
            wm.observe_event(event);
            if (!clipboard.handle(event) && !dnd.handle(event) && !startup.handle(event) &&
                !xsettings.handle(event) && !notifications.handle(event) &&
                !menu_popup.handle(event) &&
                !shell.handle(event) &&
                !tray.handle(event) && !status_panel.handle(event)) {
              wm.handle(event);
              const auto event_type = event->response_type & ~0x80;
              const auto* randr = xcb_get_extension_data(connection, &xcb_randr_id);
              const bool randr_layout_event = randr && randr->present &&
                  (event_type == randr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
                   event_type == randr->first_event + XCB_RANDR_NOTIFY);
              if (randr_layout_event ||
                  (event_type == XCB_CONFIGURE_NOTIFY &&
                   reinterpret_cast<xcb_configure_notify_event_t*>(event)->window == screen->root))
                menu_popup.refresh_geometry();
            }
            std::free(event);
          }
          while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
          if (!screensaver_checked && std::chrono::steady_clock::now() >= screensaver_check) {
            screensaver_checked = true;
            if (start_xscreensaver) midnight::launch_xscreensaver(connection, screen->root);
          }
          if (auto config = config_reload.poll()) {
            shell.apply_config(std::move(*config));
            compositor.set_enabled(shell.theme().compositing);
            wm.refresh_theme();
            desktop_services.refresh(shell.theme());
            xsettings.set_dark_mode(shell.theme().dark_mode);
            tray.refresh();
            status_panel.refresh();
            notifications.refresh();
            menu_popup.refresh_geometry();
          }
          applications.tick();
          reaper.tick();
          wm.tick();
          window_reporter.tick([&wm] { return wm.application_windows(); });
          if (auto logout = window_reporter.logout_result()) {
            if (*logout) running = false;
            else shell.show_session_notice("Logout cancelled: an application cancelled, failed, or did not respond.");
          }
          desktop_services.tick();
          shell.tick();
          tray.tick();
          status_panel.tick();
          dnd.tick();
          clipboard.tick();
          notifications.tick();
          startup.tick();
          xsettings.tick();
          compositor.tick();
          xcb_flush(connection);
          if (poll(&display, 1, (shell.menu_animation_active() || wm.resize_pending()) ? 16 : 100) < 0 && errno != EINTR) break;
        }
        if (xcb_connection_has_error(connection)) result = 1;
        if (!result && !running) window_reporter.finish(wm.application_windows());
        if (!result && !running && shell.theme().save_session) {
          // Revalidate even if saving was only enabled by live configuration.
          if (auto existing = session.load(); !existing)
            std::cerr << "Folder session not saved: " << existing.error() << '\n';
          else if (auto saved = session.save(shell.session_snapshot()); !saved)
            std::cerr << "Folder session not saved: " << saved.error() << '\n';
        }
      }
    }
  }
  xcb_disconnect(connection);
  return result;
}
