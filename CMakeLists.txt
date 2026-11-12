# Swell: a native app (.e16app) for the E16 Launcher. Cross-compiled with
# the launcher's app SDK (external/e16-sdk-examples, a submodule):
#
#   cmake -S . -B build && cmake --build build     # build/swell.e16app
#
# The host tests are a separate project: test/CMakeLists.txt (or `make test`).
cmake_minimum_required(VERSION 3.20)
set(E16_LAUNCHER_DIR ${CMAKE_CURRENT_LIST_DIR}/external/e16-sdk-examples)
if(NOT EXISTS ${E16_LAUNCHER_DIR}/apps/sdk/cmake/e16app.cmake)
    message(FATAL_ERROR "external/e16-sdk-examples is missing: git submodule update --init --recursive")
endif()
set(CMAKE_TOOLCHAIN_FILE ${E16_LAUNCHER_DIR}/external/E16-SDK/cmake/arm-none-eabi.cmake)
project(swell C)
set(CMAKE_C_STANDARD 11)
# The toolchain file links firmware against newlib-nano; apps link against
# libgcc only (e16app.cmake).
set(CMAKE_EXE_LINKER_FLAGS "")
include(${E16_LAUNCHER_DIR}/apps/sdk/cmake/e16app.cmake)

e16_add_app(swell
    src/app.c src/ui.c
    src/core/model.c src/core/store.c src/core/engine.c src/core/out.c src/core/proto.c src/core/fields.c
    ${E16_LAUNCHER_DIR}/src/ui/font.c)  # the launcher's 5x7 font (freestanding)
target_include_directories(swell.elf PRIVATE src src/core ${E16_LAUNCHER_DIR}/src/ui)
