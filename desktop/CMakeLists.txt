# Phase 1 desktop binary (macOS / Linux).

# RtMidi for MIDI output. Prefer the CMake config package; fall back to a
# manual search so Homebrew's pkgconfig-only install still works.
find_package(RtMidi QUIET CONFIG)

if(NOT RtMidi_FOUND)
    find_path(RTMIDI_INCLUDE_DIR RtMidi.h
        HINTS
            /opt/homebrew/include
            /opt/homebrew/include/rtmidi
            /usr/local/include
            /usr/local/include/rtmidi
            /usr/include
            /usr/include/rtmidi
    )
    find_library(RTMIDI_LIBRARY rtmidi
        HINTS
            /opt/homebrew/lib
            /usr/local/lib
            /usr/lib
    )
    if(NOT RTMIDI_INCLUDE_DIR OR NOT RTMIDI_LIBRARY)
        message(FATAL_ERROR
            "RtMidi not found. Install it (`brew install rtmidi` on macOS, "
            "`apt install librtmidi-dev` on Debian/Ubuntu).")
    endif()

    add_library(RtMidi::rtmidi UNKNOWN IMPORTED)
    set_target_properties(RtMidi::rtmidi PROPERTIES
        IMPORTED_LOCATION "${RTMIDI_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${RTMIDI_INCLUDE_DIR}")
endif()

# libpcap for reading captures in the replay tool. macOS ships libpcap as
# part of the Xcode SDK; on Linux it usually needs `apt install libpcap-dev`.
find_path(PCAP_INCLUDE_DIR pcap.h
    HINTS
        /opt/homebrew/include
        /usr/local/include
        /usr/include
)
find_library(PCAP_LIBRARY pcap
    HINTS
        /opt/homebrew/lib
        /usr/local/lib
        /usr/lib
)
if(NOT PCAP_LIBRARY)
    message(FATAL_ERROR
        "libpcap not found. On macOS install the Xcode command-line tools "
        "(`xcode-select --install`); on Debian/Ubuntu `apt install libpcap-dev`.")
endif()

# Optional GUI (Dear ImGui + GLFW). The bridge runs headless without this;
# the --visualize flag is only available when XDJ_GUI is on.
option(XDJ_GUI "Build the ImGui-based visualizer panel" ON)
set(XDJ_GUI_AVAILABLE OFF)

if(XDJ_GUI)
    find_package(glfw3 QUIET CONFIG)
    if(NOT glfw3_FOUND)
        find_path(GLFW_INCLUDE_DIR GLFW/glfw3.h
            HINTS /opt/homebrew/include /usr/local/include /usr/include)
        find_library(GLFW_LIBRARY glfw
            HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
        if(GLFW_INCLUDE_DIR AND GLFW_LIBRARY)
            add_library(glfw INTERFACE)
            target_include_directories(glfw INTERFACE ${GLFW_INCLUDE_DIR})
            target_link_libraries(glfw INTERFACE ${GLFW_LIBRARY})
            set(glfw3_FOUND TRUE)
        endif()
    endif()

    if(glfw3_FOUND)
        # Pull Dear ImGui at configure time. Pinned tag so behavior is stable.
        include(FetchContent)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG        v1.91.5
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(imgui)

        add_library(imgui_lib STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        )
        target_include_directories(imgui_lib PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends)
        target_link_libraries(imgui_lib PUBLIC glfw)
        # ImGui's own internal warnings are not actionable for us.
        target_compile_options(imgui_lib PRIVATE -w)
        if(APPLE)
            target_link_libraries(imgui_lib PUBLIC "-framework OpenGL"
                                                   "-framework Cocoa"
                                                   "-framework IOKit")
        else()
            find_package(OpenGL REQUIRED)
            target_link_libraries(imgui_lib PUBLIC OpenGL::GL)
        endif()
        set(XDJ_GUI_AVAILABLE ON)
    else()
        message(WARNING
            "GLFW not found — building without the ImGui visualizer. "
            "Install it (`brew install glfw`) to enable --visualize.")
    endif()
endif()

# Shared platform-layer pieces.
set(DESKTOP_IO_SOURCES
    udp_posix.cpp
    midi_rtmidi.cpp
    timer_posix.cpp
    iface_posix.cpp
    config_posix.cpp)

if(XDJ_GUI_AVAILABLE)
    list(APPEND DESKTOP_IO_SOURCES gui_imgui.cpp)
endif()

add_library(desktop_io STATIC ${DESKTOP_IO_SOURCES})
target_include_directories(desktop_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(desktop_io PUBLIC prolink)

if(TARGET RtMidi::rtmidi)
    target_link_libraries(desktop_io PUBLIC RtMidi::rtmidi)
elseif(TARGET RtMidi::RtMidi)
    target_link_libraries(desktop_io PUBLIC RtMidi::RtMidi)
endif()

if(XDJ_GUI_AVAILABLE)
    target_link_libraries(desktop_io PUBLIC imgui_lib)
    target_compile_definitions(desktop_io PUBLIC XDJ_HAVE_GUI=1)
endif()

if(APPLE)
    target_link_libraries(desktop_io PUBLIC "-framework CoreMIDI"
                                            "-framework CoreAudio"
                                            "-framework CoreFoundation")
endif()

# xdj_bridge — the actual Phase-1 binary.
add_executable(xdj_bridge main.cpp)
target_link_libraries(xdj_bridge PRIVATE desktop_io prolink)

# xdj_replay — pcapng playback to localhost for offline dev.
add_executable(xdj_replay replay.cpp)
target_link_libraries(xdj_replay PRIVATE prolink ${PCAP_LIBRARY})
target_include_directories(xdj_replay PRIVATE ${PCAP_INCLUDE_DIR})
