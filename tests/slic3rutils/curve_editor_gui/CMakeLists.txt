# Built with the other tests from tests/slic3rutils, and on its own for a quick
# check of the curve editor without building the application:
#   cmake -S tests/slic3rutils/curve_editor_gui -B <build dir> -DCMAKE_PREFIX_PATH=<deps install>
get_filename_component(ORCA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # What the top-level project and tests/CMakeLists.txt provide to an in-tree build.
    cmake_minimum_required(VERSION 3.20)
    project(curve_editor_gui_checks LANGUAGES CXX)
    set(CURVE_STANDALONE TRUE)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    # The dependencies use the DLL runtime, the debug one in Debug.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    enable_testing()
    list(APPEND CMAKE_MODULE_PATH "${ORCA_SOURCE_DIR}/tests/catch2/extras")
    include(Catch)
endif()

# Match the application's discovery mode. System Linux installations may only
# provide wx-config; wx::* CONFIG targets are available in the other deps builds.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(DEFINED SLIC3R_STATIC)
        set(wxWidgets_USE_STATIC "${SLIC3R_STATIC}")
    endif()
    if(DEFINED SLIC3R_GTK)
        set(wxWidgets_CONFIG_OPTIONS "--toolkit=gtk${SLIC3R_GTK}")
    endif()
    find_package(wxWidgets MODULE REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})
else()
    find_package(wxWidgets CONFIG REQUIRED COMPONENTS core base)
endif()
find_package(Boost CONFIG REQUIRED)

# Use the same vendored Catch2 version without rebuilding the main application.
if(CURVE_STANDALONE)
    add_library(curve_editor_catch STATIC "${ORCA_SOURCE_DIR}/tests/catch2/extras/catch_amalgamated.cpp")
    target_include_directories(curve_editor_catch PUBLIC "${ORCA_SOURCE_DIR}/tests/catch2/extras")
else()
    add_library(curve_editor_catch INTERFACE)
    target_link_libraries(curve_editor_catch INTERFACE Catch2::Catch2WithMain)
    target_include_directories(curve_editor_catch INTERFACE "${ORCA_SOURCE_DIR}/tests/catch2/extras")
endif()
# Compile the production interaction/geometry source directly. The fixture links
# an empty paint implementation in place of the application's theme-dependent one.
add_executable(curve_editor_gui_tests test_curve_editor.cpp curve_editor_panel_paint_stub.cpp
    "${ORCA_SOURCE_DIR}/src/slic3r/GUI/CurveEditorPanel.cpp"
    "${ORCA_SOURCE_DIR}/src/libslic3r/CurveModel.cpp"
    "${ORCA_SOURCE_DIR}/src/libslic3r/LocalesUtils.cpp"
    "${ORCA_SOURCE_DIR}/src/libslic3r/GCode/PchipInterpolatorHelper.cpp")
target_include_directories(curve_editor_gui_tests PRIVATE "${ORCA_SOURCE_DIR}/src"
    "${ORCA_SOURCE_DIR}/src/slic3r/GUI")
target_include_directories(curve_editor_gui_tests SYSTEM PRIVATE "${ORCA_SOURCE_DIR}/deps_src")
target_compile_definitions(curve_editor_gui_tests PRIVATE wxDEBUG_LEVEL=0)
target_link_libraries(curve_editor_gui_tests PRIVATE curve_editor_catch ${wxWidgets_LIBRARIES} Boost::headers)
set_property(TARGET curve_editor_gui_tests PROPERTY FOLDER "tests")

if(APPLE)
    # Like the application, use the CXX compiler's .mm handling and CXX flags.
    target_sources(curve_editor_gui_tests PRIVATE "${ORCA_SOURCE_DIR}/src/slic3r/GUI/CurveEditorPanelMac.mm")
    target_link_libraries(curve_editor_gui_tests PRIVATE "-framework AppKit")
elseif(UNIX)
    # The GTK version wxWidgets is built for (SLIC3R_GTK of the top-level project, 3 by default).
    set(_curve_gtk_version 3)
    if(SLIC3R_GTK)
        set(_curve_gtk_version "${SLIC3R_GTK}")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CURVE_GTK REQUIRED IMPORTED_TARGET gtk+-${_curve_gtk_version}.0)
    target_link_libraries(curve_editor_gui_tests PRIVATE PkgConfig::CURVE_GTK)
endif()

# Pointer capture belongs to the display, so parallel processes must not steal it.
set(_curve_test_properties RESOURCE_LOCK curve_editor_display)
if(UNIX AND NOT APPLE)
    # Headless GTK checks do not use AT-SPI; an absent accessibility bus can stall startup.
    list(APPEND _curve_test_properties ENVIRONMENT NO_AT_BRIDGE=1)
endif()
# The cases that open windows have the [RequiresDisplay] tag, which becomes a ctest label:
# scripts/run_unit_tests.sh leaves them out on a Linux machine with neither a display nor Xvfb.
if(CURVE_STANDALONE)
    catch_discover_tests(curve_editor_gui_tests ADD_TAGS_AS_LABELS EXTRA_ARGS --warn NoAssertions
        PROPERTIES ${_curve_test_properties})
else()
    orcaslicer_discover_tests(curve_editor_gui_tests PROPERTIES ${_curve_test_properties})
endif()
