# Unit tests for the portable core. Deliberately dependency-free (no gtest) so
# they build anywhere lib/prolink does, and so CI needs no package installs
# beyond a compiler.

add_executable(xdj_tests
    test_prolink.cpp
    test_bridge.cpp
)
target_link_libraries(xdj_tests PRIVATE prolink)

# The core is meant to be warning-clean; tests hold it to that.
target_compile_options(xdj_tests PRIVATE -Wall -Wextra)

add_test(NAME prolink COMMAND xdj_tests)
