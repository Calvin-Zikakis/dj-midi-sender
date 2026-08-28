# Unit tests for the portable core. Deliberately dependency-free (no gtest) so
# they build anywhere lib/prolink does, and so CI needs no package installs
# beyond a compiler.

add_executable(xdj_tests
    test_prolink.cpp
    test_bridge.cpp
    test_ui.cpp
)
target_link_libraries(xdj_tests PRIVATE prolink)

# firmware/src/ui_display.hpp is header-only apart from two label functions, so
# the source-gating helpers can be tested on the host without pulling in any
# Arduino/ESP-IDF dependency.
target_include_directories(xdj_tests PRIVATE ${CMAKE_SOURCE_DIR}/firmware/src)

# The core is meant to be warning-clean; tests hold it to that.
target_compile_options(xdj_tests PRIVATE -Wall -Wextra)

add_test(NAME prolink COMMAND xdj_tests)

# The real POSIX socket layer. Separate binary because it compiles a desktop
# source file directly — it needs no RtMidi or libpcap, so it still runs in CI
# where the full desktop build is switched off.
if(UNIX)
    add_executable(xdj_udp_tests
        test_udp_posix.cpp
        ${CMAKE_SOURCE_DIR}/desktop/udp_posix.cpp
    )
    target_link_libraries(xdj_udp_tests PRIVATE prolink)
    target_include_directories(xdj_udp_tests PRIVATE ${CMAKE_SOURCE_DIR}/desktop)
    target_compile_options(xdj_udp_tests PRIVATE -Wall -Wextra)
    add_test(NAME udp_posix COMMAND xdj_udp_tests)
endif()
