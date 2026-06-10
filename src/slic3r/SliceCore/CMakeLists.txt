cmake_minimum_required(VERSION 3.13)
project(liborca_slice_core)

add_library(liborca_slice_core STATIC
    SliceService.cpp
    PresetResolver.cpp
    OutputTarget.cpp
)

# Expose the SliceCore directory so consumers can include headers
# with the short form: #include "SliceTypes.hpp" etc.
target_include_directories(liborca_slice_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# libslic3r is the core slicing library; its PUBLIC include dirs expose
# the src/ root, which is what lets us write #include "libslic3r/PrintConfig.hpp".
# libslic3r_gui is included for future use (HTTP server, print-host delivery).
target_link_libraries(liborca_slice_core
    PUBLIC
        libslic3r
        libslic3r_gui
)
