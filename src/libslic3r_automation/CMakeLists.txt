cmake_minimum_required(VERSION 3.13)
project(libslic3r_automation)

# The automation core: the command catalog's type, the dispatch core, the hand-off to the UI thread, the job
# registry, the followed threads, the journal, the paths, argument checking, the protocol and HTTP transport,
# what the server says of itself, the replay model, Action Replay's summary of a replay, the setup a client
# copies, the stdio bridge and the token.
# It uses no wxWidgets and nothing of libslic3r_gui, which builds the automation runtime, the MCP
# endpoint and the tool families on top of it.
#
# The sources stay beside the GUI code that uses them, under src/slic3r/GUI. The build keeps wx out:
# src/CMakeLists.txt adds this directory before it finds wxWidgets. On Linux, wx's include paths and
# definitions apply at directory scope only from the point after which this library is added. On Windows
# and macOS, wx's setup header and definitions come only with the wx targets, which this library does not
# link. So a wx include in a core unit fails to compile. The core's tests (tests/automation) link this
# library and not libslic3r_gui, so a call from a core unit into the GUI library fails to link there.
set(SLIC3R_AUTOMATION_SOURCES
    ActionIds.hpp
    CommandCatalog.cpp
    CommandCatalog.hpp
    CommandTypes.hpp
    Mcp/FollowedThreads.cpp
    Mcp/FollowedThreads.hpp
    Mcp/JobRegistry.cpp
    Mcp/JobRegistry.hpp
    Mcp/McpArguments.cpp
    Mcp/McpArguments.hpp
    Mcp/McpCore.cpp
    Mcp/McpCore.hpp
    Mcp/McpHttpServer.cpp
    Mcp/McpHttpServer.hpp
    Mcp/McpJournal.cpp
    Mcp/McpJournal.hpp
    Mcp/McpOptions.hpp
    Mcp/McpPaths.cpp
    Mcp/McpPaths.hpp
    Mcp/McpProtocol.cpp
    Mcp/McpProtocol.hpp
    Mcp/McpReplay.cpp
    Mcp/McpReplay.hpp
    Mcp/McpReplaySummary.cpp
    Mcp/McpReplaySummary.hpp
    Mcp/McpServerInfo.cpp
    Mcp/McpSetup.cpp
    Mcp/McpSetup.hpp
    Mcp/McpStdioBridge.cpp
    Mcp/McpStdioBridge.hpp
    Mcp/McpToken.cpp
    Mcp/McpToken.hpp
    Mcp/UiInvoker.cpp
    Mcp/UiInvoker.hpp
    Mcp/WindowNames.hpp
)

get_filename_component(_gui_dir ${CMAKE_CURRENT_SOURCE_DIR}/../slic3r/GUI ABSOLUTE)
list(TRANSFORM SLIC3R_AUTOMATION_SOURCES PREPEND ${_gui_dir}/)

add_library(libslic3r_automation STATIC ${SLIC3R_AUTOMATION_SOURCES})
source_group(TREE ${_gui_dir} FILES ${SLIC3R_AUTOMATION_SOURCES})

encoding_check(libslic3r_automation)

target_link_libraries(libslic3r_automation PUBLIC libslic3r)

# McpPaths.cpp calls CoreFoundation on macOS.
if (APPLE)
    find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
    target_link_libraries(libslic3r_automation PRIVATE ${CORE_FOUNDATION})
endif ()
