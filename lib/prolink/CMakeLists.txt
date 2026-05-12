# Pure C++17 core. No system headers beyond <cstdint>, <cstring>,
# <functional>, <optional>, <atomic>. This builds for any target without
# changes — desktop or ESP32-S3.
add_library(prolink STATIC
    packets.cpp
    clock.cpp
    bridge.cpp
)

target_include_directories(prolink PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(prolink PUBLIC cxx_std_17)
