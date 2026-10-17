cmake_minimum_required(VERSION 3.25)
project(midnight-wm VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  xcb xcb-icccm xcb-keysyms xcb-xfixes xcb-randr cairo-xcb pangocairo
  gio-2.0 gio-unix-2.0 gdk-pixbuf-2.0 sqlite3)

add_library(midnight_core src/store.cpp src/model.cpp src/config.cpp src/audio.cpp
  src/desktop_launcher.cpp src/thumbnail.cpp)
target_include_directories(midnight_core PUBLIC src)
target_link_libraries(midnight_core PUBLIC PkgConfig::DEPS)
target_compile_options(midnight_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(midnight-wm src/main.cpp src/x11.cpp src/shell.cpp src/clipboard.cpp src/dnd.cpp
  src/tray.cpp src/status_notifier.cpp src/status_panel.cpp src/dbus_menu.cpp
  src/dbus_menu_popup.cpp src/autostart.cpp src/notifications.cpp)
target_link_libraries(midnight-wm PRIVATE midnight_core)
target_compile_options(midnight-wm PRIVATE -Wall -Wextra -Wpedantic)

include(GNUInstallDirs)
target_compile_definitions(midnight-wm PRIVATE
  MIDNIGHT_WM_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/midnight-wm"
  MIDNIGHT_WM_VERSION="${PROJECT_VERSION}")
install(TARGETS midnight-wm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/midnight-wm.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/xsessions)
install(FILES data/config.ini data/MidnightBSDLogo.svg data/MidnightBSDLogo22x22.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/midnight-wm)
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/midnight-wm)

enable_testing()
add_executable(store_test tests/store_test.cpp)
target_link_libraries(store_test PRIVATE midnight_core)
add_test(NAME store_test COMMAND store_test)
add_executable(model_test tests/model_test.cpp)
target_link_libraries(model_test PRIVATE midnight_core)
add_test(NAME model_test COMMAND model_test)
add_executable(autostart_test tests/autostart_test.cpp src/autostart.cpp)
target_link_libraries(autostart_test PRIVATE midnight_core)
add_test(NAME autostart_test COMMAND autostart_test)
add_executable(desktop_launcher_test tests/desktop_launcher_test.cpp)
target_link_libraries(desktop_launcher_test PRIVATE midnight_core)
add_test(NAME desktop_launcher_test COMMAND desktop_launcher_test)
add_executable(thumbnail_test tests/thumbnail_test.cpp)
target_link_libraries(thumbnail_test PRIVATE midnight_core)
add_test(NAME thumbnail_test COMMAND thumbnail_test)

set(CPACK_PACKAGE_NAME "midnight-wm")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "midnight-wm-${PROJECT_VERSION}")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES "/build/;/.git/;/.cache/;/_CPack_Packages/;[.]core$;/packaging/mports/.*/distinfo$;/packaging/mports/.*/work/;[.]tar[.]gz$")
include(CPack)
