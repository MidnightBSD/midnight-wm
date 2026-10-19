cmake_minimum_required(VERSION 3.25)
project(midnight-wm VERSION 0.1.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  xcb xcb-icccm xcb-keysyms xcb-xfixes xcb-randr xcb-xinerama xcb-sync xcb-screensaver cairo-xcb pangocairo
  gio-2.0 gio-unix-2.0 gdk-pixbuf-2.0 garcon-1 sqlite3)

add_library(midnight_core src/folder_view.cpp src/content_size.cpp src/store.cpp src/model.cpp src/config.cpp src/session.cpp src/private_state.cpp src/session_bus.cpp src/application_session.cpp src/application_checkpoint.cpp src/session_windows.cpp src/application_restore.cpp src/audio.cpp src/battery.cpp src/network.cpp src/wallpaper.cpp
  src/desktop_launcher.cpp src/thumbnail.cpp src/recent_files.cpp
  src/startup_notification.cpp src/browser_launcher.cpp src/terminal_launcher.cpp src/task_icon.cpp)
target_include_directories(midnight_core PUBLIC src)
target_link_libraries(midnight_core PUBLIC PkgConfig::DEPS)
if(CMAKE_SYSTEM_NAME MATCHES "^(MidnightBSD|FreeBSD)$")
  target_link_libraries(midnight_core PUBLIC util)
endif()
target_compile_options(midnight_core PRIVATE -Wall -Wextra -Wpedantic)

pkg_check_modules(XSMP REQUIRED IMPORTED_TARGET sm ice)
add_library(midnight_xsmp STATIC src/xsmp_properties.cpp)
target_link_libraries(midnight_xsmp PUBLIC midnight_core PkgConfig::XSMP)
target_compile_options(midnight_xsmp PRIVATE -Wall -Wextra -Wpedantic)

add_executable(midnight-wm src/main.cpp src/x11.cpp src/shell.cpp src/clipboard.cpp src/dnd.cpp
  src/tray.cpp src/status_notifier.cpp src/status_panel.cpp src/dbus_menu.cpp
  src/dbus_menu_popup.cpp src/autostart.cpp src/notifications.cpp src/xsettings.cpp
  src/file_manager1.cpp src/keyring_session.cpp src/media_player.cpp src/desktop_services.cpp
  src/compositor.cpp)
target_link_libraries(midnight-wm PRIVATE midnight_core)
if(CMAKE_SYSTEM_NAME MATCHES "^(MidnightBSD|FreeBSD)$")
  target_link_libraries(midnight-wm PRIVATE util)
endif()
target_compile_options(midnight-wm PRIVATE -Wall -Wextra -Wpedantic)

pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
add_executable(midnightbsd-settings src/settings.cpp)
target_link_libraries(midnightbsd-settings PRIVATE midnight_core PkgConfig::GTK3)
target_compile_options(midnightbsd-settings PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(midnightbsd-settings PRIVATE MIDNIGHT_WM_VERSION="${PROJECT_VERSION}")

add_executable(midnightbsd-search src/search.cpp)
target_link_libraries(midnightbsd-search PRIVATE PkgConfig::GTK3)
target_compile_options(midnightbsd-search PRIVATE -Wall -Wextra -Wpedantic)

include(GNUInstallDirs)
target_compile_definitions(midnightbsd-settings PRIVATE
  MIDNIGHT_WM_BACKGROUND_DIR="${CMAKE_INSTALL_FULL_DATADIR}/backgrounds/midnightbsd")
target_compile_definitions(midnight_core PRIVATE
  MIDNIGHT_WM_BACKGROUND_DIR="${CMAKE_INSTALL_FULL_DATADIR}/backgrounds/midnightbsd")
target_compile_definitions(midnight-wm PRIVATE
  MIDNIGHT_WM_SEARCH_PROGRAM="${CMAKE_INSTALL_FULL_BINDIR}/midnightbsd-search"
  MIDNIGHT_WM_SETTINGS_PROGRAM="${CMAKE_INSTALL_FULL_BINDIR}/midnightbsd-settings"
  MIDNIGHT_WM_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/midnight-wm"
  MIDNIGHT_WM_VERSION="${PROJECT_VERSION}")
add_executable(midnightbsd-session src/session_main.cpp src/session_manager.cpp)
find_package(Threads REQUIRED)
target_link_libraries(midnightbsd-session PRIVATE midnight_xsmp Threads::Threads)
target_compile_options(midnightbsd-session PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(midnightbsd-session PRIVATE
  MIDNIGHT_WM_VERSION="${PROJECT_VERSION}"
  MIDNIGHT_WM_PROGRAM="${CMAKE_INSTALL_FULL_BINDIR}/midnight-wm")
install(TARGETS midnight-wm midnightbsd-search midnightbsd-settings midnightbsd-session RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/midnightbsd-settings.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/midnight-wm.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/xsessions)
install(FILES data/config.ini data/MidnightBSDLogo.svg data/MidnightBSDLogo22x22.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/midnight-wm)
install(FILES data/midnight.portal DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals)
install(FILES data/midnightworkspace-portals.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/midnight-wm)

enable_testing()
add_executable(folder_view_test tests/folder_view_test.cpp)
target_link_libraries(folder_view_test PRIVATE midnight_core)
add_test(NAME folder_view_test COMMAND folder_view_test)
add_executable(content_size_test tests/content_size_test.cpp)
target_link_libraries(content_size_test PRIVATE midnight_core)
add_test(NAME content_size_test COMMAND content_size_test)
add_executable(x11_timestamp_test tests/x11_timestamp_test.cpp)
target_include_directories(x11_timestamp_test PRIVATE src)
add_test(NAME x11_timestamp_test COMMAND x11_timestamp_test)
add_library(battery_sysctl_fixture SHARED tests/battery_sysctl_fixture.cpp)
target_link_libraries(battery_sysctl_fixture PRIVATE ${CMAKE_DL_LIBS})
add_executable(battery_test tests/battery_test.cpp)
target_link_libraries(battery_test PRIVATE midnight_core)
add_test(NAME battery_test COMMAND battery_test)
find_program(XVFB Xvfb)
find_package(Python3 COMPONENTS Interpreter)
if(XVFB AND Python3_Interpreter_FOUND)
  add_executable(compositor_test tests/compositor_test.cpp src/compositor.cpp)
  target_link_libraries(compositor_test PRIVATE midnight_core)
  add_test(NAME compositor_test COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/security_x11_test.py $<TARGET_FILE:compositor_test>)
  set_tests_properties(compositor_test PROPERTIES TIMEOUT 25)
  add_executable(security_x11_test tests/security_x11_test.cpp src/clipboard.cpp src/dnd.cpp)
  target_link_libraries(security_x11_test PRIVATE midnight_core)
  add_test(NAME security_x11_test COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/security_x11_test.py $<TARGET_FILE:security_x11_test>)
  set_tests_properties(security_x11_test PROPERTIES TIMEOUT 25)
endif()

add_executable(wallpaper_test tests/wallpaper_test.cpp)
target_link_libraries(wallpaper_test PRIVATE midnight_core)
add_test(NAME wallpaper_test COMMAND wallpaper_test)
add_executable(task_icon_test tests/task_icon_test.cpp)
target_link_libraries(task_icon_test PRIVATE midnight_core)
add_test(NAME task_icon_test COMMAND task_icon_test)
add_executable(config_test tests/config_test.cpp)
target_link_libraries(config_test PRIVATE midnight_core)
add_test(NAME config_test COMMAND config_test)
add_executable(screen_resize_event tests/screen_resize_event.cpp)
target_link_libraries(screen_resize_event PRIVATE PkgConfig::DEPS)
add_executable(display_monitors_test tests/display_monitors_test.cpp)
target_include_directories(display_monitors_test PRIVATE src)
target_link_libraries(display_monitors_test PRIVATE PkgConfig::DEPS)
add_test(NAME display_monitors_test COMMAND display_monitors_test)
add_executable(store_test tests/store_test.cpp)
target_link_libraries(store_test PRIVATE midnight_core)
add_test(NAME store_test COMMAND store_test)
add_executable(network_test tests/network_test.cpp)
target_link_libraries(network_test PRIVATE midnight_core)
add_test(NAME network_test COMMAND network_test)
add_executable(session_test tests/session_test.cpp)
target_link_libraries(session_test PRIVATE midnight_core)
add_test(NAME session_test COMMAND session_test)

add_executable(session_windows_test tests/session_windows_test.cpp)
target_link_libraries(session_windows_test PRIVATE midnight_core Threads::Threads)
add_test(NAME session_windows_test COMMAND session_windows_test)

add_executable(application_restore_test tests/application_restore_test.cpp)
target_link_libraries(application_restore_test PRIVATE midnight_core)
add_test(NAME application_restore_test COMMAND application_restore_test)

add_executable(application_checkpoint_test tests/application_checkpoint_test.cpp)
target_link_libraries(application_checkpoint_test PRIVATE midnight_core)
add_test(NAME application_checkpoint_test COMMAND application_checkpoint_test)

add_executable(xsmp_properties_test tests/xsmp_properties_test.cpp)
target_link_libraries(xsmp_properties_test PRIVATE midnight_xsmp)
add_test(NAME xsmp_properties_test COMMAND xsmp_properties_test)
add_executable(xsmp_restore_fixture tests/xsmp_restore_fixture.cpp)
target_link_libraries(xsmp_restore_fixture PRIVATE PkgConfig::XSMP PkgConfig::DEPS)
add_executable(desktop_relaunch_fixture tests/desktop_relaunch_fixture.cpp)
target_link_libraries(desktop_relaunch_fixture PRIVATE PkgConfig::DEPS)

add_executable(xsmp_logout_fixture tests/xsmp_logout_fixture.cpp)
target_link_libraries(xsmp_logout_fixture PRIVATE PkgConfig::XSMP)

add_executable(xsmp_client_fixture tests/xsmp_client.cpp)
target_link_libraries(xsmp_client_fixture PRIVATE PkgConfig::XSMP)

add_executable(model_test tests/model_test.cpp)
target_link_libraries(model_test PRIVATE midnight_core)
add_test(NAME model_test COMMAND model_test)
add_executable(autostart_test tests/autostart_test.cpp src/autostart.cpp)
target_link_libraries(autostart_test PRIVATE midnight_core)
add_test(NAME autostart_test COMMAND autostart_test)
add_executable(keyring_session_test tests/keyring_session_test.cpp src/keyring_session.cpp)
target_link_libraries(keyring_session_test PRIVATE midnight_core)
add_test(NAME keyring_inherited_test COMMAND keyring_session_test inherited)
add_test(NAME keyring_fallback_test COMMAND keyring_session_test fallback)
set_tests_properties(keyring_fallback_test PROPERTIES SKIP_RETURN_CODE 77)
add_executable(desktop_launcher_test tests/desktop_launcher_test.cpp)
target_link_libraries(desktop_launcher_test PRIVATE midnight_core)
add_test(NAME desktop_launcher_test COMMAND desktop_launcher_test)
add_executable(desktop_layout_test tests/desktop_layout_test.cpp)
target_include_directories(desktop_layout_test PRIVATE src)
add_test(NAME desktop_layout_test COMMAND desktop_layout_test)
add_executable(browser_launcher_test tests/browser_launcher_test.cpp)
target_link_libraries(browser_launcher_test PRIVATE midnight_core)
add_test(NAME browser_fallback_test COMMAND browser_launcher_test fallback)
add_test(NAME browser_default_test COMMAND browser_launcher_test default)
add_executable(terminal_launcher_test tests/terminal_launcher_test.cpp)
target_link_libraries(terminal_launcher_test PRIVATE midnight_core)
add_test(NAME terminal_fallback_test COMMAND terminal_launcher_test fallback)
add_test(NAME terminal_default_test COMMAND terminal_launcher_test default)
add_test(NAME terminal_env_test COMMAND terminal_launcher_test env)
add_executable(thumbnail_test tests/thumbnail_test.cpp)
target_link_libraries(thumbnail_test PRIVATE midnight_core)
add_test(NAME thumbnail_test COMMAND thumbnail_test)
add_executable(recent_files_test tests/recent_files_test.cpp)
target_link_libraries(recent_files_test PRIVATE midnight_core)
add_test(NAME recent_files_test COMMAND recent_files_test)
set(MIDNIGHT_WM_RANDR_MODULE_PATH "" CACHE PATH "Private Xorg dummy 0.4.x module root for optional RandR smoke checks")
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
  if(Python3_Interpreter_FOUND)
    add_test(NAME xsmp_transport_test COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/xsmp_smoke.py
      --binary $<TARGET_FILE:midnightbsd-session>
      --fixture $<TARGET_FILE:xsmp_client_fixture>
      --output ${CMAKE_CURRENT_BINARY_DIR}/xsmp-test-results)
    set_tests_properties(xsmp_transport_test PROPERTIES TIMEOUT 140)
    if(MIDNIGHT_WM_RANDR_MODULE_PATH)
      find_program(XORG_SERVER Xorg REQUIRED)
      find_program(XRANDR xrandr REQUIRED)
      find_program(XAUTH xauth REQUIRED)
      if(NOT EXISTS "${MIDNIGHT_WM_RANDR_MODULE_PATH}/drivers/dummy_drv.so")
        message(FATAL_ERROR "RandR smoke checks require a dummy 0.4.x module in drivers/dummy_drv.so")
      endif()
      add_test(NAME session_randr_smoke COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/session_randr_smoke.py
        --module-path ${MIDNIGHT_WM_RANDR_MODULE_PATH}
        --binary $<TARGET_FILE:midnightbsd-session> --fixture $<TARGET_FILE:xsmp_restore_fixture>)
      set_tests_properties(session_randr_smoke PROPERTIES TIMEOUT 130)
    endif()
    if(XVFB)
      add_test(NAME application_restore_smoke COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/application_restore_smoke.py
        --binary $<TARGET_FILE:midnightbsd-session> --fixture $<TARGET_FILE:xsmp_restore_fixture>
        --output ${CMAKE_CURRENT_BINARY_DIR}/application-restore-results)
      set_tests_properties(application_restore_smoke PROPERTIES TIMEOUT 100)
      find_program(WMCTRL wmctrl)
      find_program(XDOTOOL xdotool)
      find_program(XPROP xprop)
      find_program(XWININFO xwininfo)
      if(WMCTRL AND XDOTOOL AND XPROP AND XWININFO)
        add_test(NAME desktop_relaunch_smoke COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/desktop_relaunch_smoke.py
          --binary $<TARGET_FILE:midnightbsd-session> --fixture $<TARGET_FILE:desktop_relaunch_fixture>)
        set_tests_properties(desktop_relaunch_smoke PROPERTIES TIMEOUT 100)
        add_test(NAME coordinated_logout_smoke COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/logout_smoke.py
          --binary $<TARGET_FILE:midnightbsd-session> --fixture $<TARGET_FILE:xsmp_logout_fixture>)
        set_tests_properties(coordinated_logout_smoke PROPERTIES TIMEOUT 100)
        add_test(NAME application_changed_resolution_smoke COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/application_restore_smoke.py --placements
          --width 3840 --height 2160 --restore-width 1280 --restore-height 720
          --binary $<TARGET_FILE:midnightbsd-session> --fixture $<TARGET_FILE:xsmp_restore_fixture>
          --output ${CMAKE_CURRENT_BINARY_DIR}/application-changed-resolution-results)
        set_tests_properties(application_changed_resolution_smoke PROPERTIES TIMEOUT 100)
        add_test(NAME application_placement_smoke COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/application_restore_smoke.py --placements
          --binary $<TARGET_FILE:midnightbsd-session> --fixture $<TARGET_FILE:xsmp_restore_fixture>
          --output ${CMAKE_CURRENT_BINARY_DIR}/application-placement-results)
        set_tests_properties(application_placement_smoke PROPERTIES TIMEOUT 100)
      endif()
    endif()
  endif()
  add_executable(desktop_services_test tests/desktop_services_test.cpp src/desktop_services.cpp)
  target_link_libraries(desktop_services_test PRIVATE midnight_core)
  add_test(NAME desktop_services_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:desktop_services_test>)
  add_test(NAME desktop_services_existing_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:desktop_services_test> existing)
  add_executable(file_manager1_test tests/file_manager1_test.cpp src/file_manager1.cpp)
  target_link_libraries(file_manager1_test PRIVATE midnight_core)
  add_test(NAME file_manager1_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:file_manager1_test>)
endif()

set(CPACK_PACKAGE_NAME "midnight-wm")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "midnight-wm-${PROJECT_VERSION}")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES "/build/;/.git/;/.cache/;/_CPack_Packages/;[.]core$;/packaging/mports/.*/distinfo$;/packaging/mports/.*/work/;[.]tar[.]gz$")
include(CPack)
