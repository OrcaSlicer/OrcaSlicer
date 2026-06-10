cmake_minimum_required(VERSION 3.13)
project(orca-server)

# orca-server: headless REST server for remote slicing jobs.
#
# Dependencies resolved transitively:
#   liborca_slice_core  PUBLIC→  libslic3r_gui  PUBLIC→  libslic3r
#                                libslic3r_gui  PUBLIC→  ${wxWidgets_LIBRARIES}
#                                libslic3r_gui  PUBLIC→  OpenSSL::SSL / Crypto
#   boost_libs          INTERFACE→ boost_headeronly + ${Boost_LIBRARIES}
#   boost_headeronly    INTERFACE→ ${Boost_INCLUDE_DIRS}  (gives beast/asio headers)
#   nlohmann_json       INTERFACE→ deps_src/nlohmann include dir
#
# Beast and Asio are header-only and accessed via boost_headeronly.
# No new third-party dependencies are introduced.

add_executable(orca-server
    OrcaServerMain.cpp
    HttpServer.cpp
    Router.cpp
    Handlers.cpp
    JobQueue.cpp
    Authenticator.cpp
    ServerContext.cpp
)

# Include the Server directory itself so #include "JobQueue.hpp" etc. resolve
# without a path prefix, mirroring how other slic3r sub-libraries are structured.
target_include_directories(orca-server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# orca-server depends on SliceCore which transitively provides libslic3r,
# libslic3r_gui, wxWidgets, and OpenSSL.
# boost_libs provides the compiled Boost libraries (system, filesystem, thread,
# log, …) plus the header-only Boost (including Beast/Asio).
# nlohmann_json provides the vendored JSON header.
target_link_libraries(orca-server
    PRIVATE
        liborca_slice_core   # SliceService + SliceTypes; pulls libslic3r + libslic3r_gui
        boost_libs           # Boost compiled libs + Beast/Asio headers (boost_headeronly)
        nlohmann_json        # deps_src/nlohmann — vendored, no new deps
)

# Windows-specific: ws2_32 is needed for Winsock (Asio uses it).
# libslic3r_gui already links it on MSVC via Setupapi.lib, but we add it
# explicitly here to be safe if the linker does not propagate it transitively
# from a STATIC lib on all MSVC versions.
if (WIN32)
    target_link_libraries(orca-server PRIVATE ws2_32.lib)
endif ()

# MSVC: generate debug symbols in release builds (consistent with OrcaSlicer).
if (MSVC)
    target_link_options(orca-server PUBLIC "$<$<CONFIG:RELEASE>:/DEBUG>")
endif ()

# Propagate the same Boost compile definitions used by the rest of the project
# so that Boost.Log dynamic/static linking mode is consistent.
# boost_headeronly already carries these as INTERFACE_COMPILE_DEFINITIONS
# and they will be inherited via boost_libs, but we list them explicitly so
# this CMakeLists.txt is self-documenting.
#
# (No target_compile_definitions call needed — inherited from boost_libs.)
