cmake_minimum_required(VERSION 3.16)
project(dj_midi_sender LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Core library — compiles unchanged on desktop AND ESP32-S3 (firmware phase).
add_subdirectory(lib/prolink)

# Phase 1 desktop binary (macOS / Linux). Needs RtMidi + libpcap, so it can be
# switched off (e.g. in CI, which only needs the core and its tests).
option(XDJ_DESKTOP "Build the desktop binaries (needs RtMidi, libpcap)" ON)
if(XDJ_DESKTOP AND NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
    add_subdirectory(desktop)
endif()

# Unit tests for lib/prolink. No external dependencies.
option(XDJ_TESTS "Build the unit tests" ON)
if(XDJ_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
