# Standalone host build for the CrossPoint UI screen renderer. Deliberately
# NOT wired into test/CMakeLists.txt (the `pio run -t unit-tests` gtest
# suite): this is a visualization tool, not a test, and keeping it separate
# means it can't perturb the unit-tests count.
#
# Build:
#   cmake -S tools/render_ui_screens -B build/render_ui_screens
#   cmake --build build/render_ui_screens
# Run:
#   ./build/render_ui_screens/render_ui_screens <output_dir>

cmake_minimum_required(VERSION 3.16)
project(render_ui_screens CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(render_ui_screens
  render_ui_screens.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/I18n/I18n.cpp
  ${REPO_ROOT}/lib/I18n/I18nStrings.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp
)

target_include_directories(render_ui_screens PRIVATE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/I18n
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
  ${REPO_ROOT}/freeink-sdk/libs/hardware/BoardConfig/include
  ${REPO_ROOT}/freeink-sdk/libs/assets/Icons/include
)

# hostshim/ stands in for <Arduino.h>/<driver/gpio.h>/<esp_rom_sys.h> so
# BoardConfig.h (which unconditionally includes them) parses on the host;
# see hostshim/Arduino.h for why this is safe. SYSTEM keeps its warnings out
# of -Wall.
target_include_directories(render_ui_screens SYSTEM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hostshim
)

target_compile_definitions(render_ui_screens PRIVATE FREEINK_DEVICE_X4=1)

target_compile_options(render_ui_screens PRIVATE -Wall -Wextra)
