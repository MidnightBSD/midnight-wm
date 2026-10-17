cmake_minimum_required(VERSION 3.25)
project(midnight-wm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  xcb xcb-icccm xcb-keysyms xcb-xfixes xcb-randr cairo-xcb pangocairo
  gio-2.0 gio-unix-2.0 sqlite3)

add_library(midnight_core src/store.cpp src/model.cpp src/config.cpp src/audio.cpp)
target_include_directories(midnight_core PUBLIC src)
target_link_libraries(midnight_core PUBLIC PkgConfig::DEPS)
target_compile_options(midnight_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(midnight-wm src/main.cpp src/x11.cpp src/shell.cpp src/clipboard.cpp src/dnd.cpp)
target_link_libraries(midnight-wm PRIVATE midnight_core)
target_compile_options(midnight-wm PRIVATE -Wall -Wextra -Wpedantic)

include(GNUInstallDirs)
install(TARGETS midnight-wm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/midnight-wm.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/xsessions)
install(FILES data/config.ini DESTINATION ${CMAKE_INSTALL_DATADIR}/midnight-wm)
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/midnight-wm)

enable_testing()
add_executable(store_test tests/store_test.cpp)
target_link_libraries(store_test PRIVATE midnight_core)
add_test(NAME store_test COMMAND store_test)
add_executable(model_test tests/model_test.cpp)
target_link_libraries(model_test PRIVATE midnight_core)
add_test(NAME model_test COMMAND model_test)

set(CPACK_PACKAGE_NAME "midnight-wm")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "midnight-wm-${PROJECT_VERSION}")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES "/build/;/.git/;/.cache/;[.]core$;/packaging/mports/.*/distinfo$;/packaging/mports/.*/work/;[.]tar[.]gz$")
include(CPack)
