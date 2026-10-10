cmake_minimum_required(VERSION 3.20)
project(curve_editor_gui_checks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
get_filename_component(ORCA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(wxWidgets CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Compile the panel's production interaction/geometry code. Painting is isolated
# because it depends on the full application, profiles and plugin startup.
set(_panel "${ORCA_SOURCE_DIR}/src/slic3r/GUI/CurveEditorPanel.cpp")
set(_test_panel "${CMAKE_CURRENT_BINARY_DIR}/CurveEditorPanel.cpp")
add_custom_command(OUTPUT "${_test_panel}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_panel.py" "${_panel}" "${_test_panel}"
    DEPENDS "${_panel}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_panel.py"
    VERBATIM)

# Use the same vendored Catch2 version without rebuilding the main application.
add_library(curve_editor_catch STATIC "${ORCA_SOURCE_DIR}/tests/catch2/extras/catch_amalgamated.cpp")
target_include_directories(curve_editor_catch PUBLIC "${ORCA_SOURCE_DIR}/tests/catch2/extras")
add_executable(curve_editor_gui_tests test_curve_editor.cpp "${_test_panel}"
    "${ORCA_SOURCE_DIR}/src/libslic3r/CurveModel.cpp"
    "${ORCA_SOURCE_DIR}/src/libslic3r/GCode/PchipInterpolatorHelper.cpp")
target_include_directories(curve_editor_gui_tests PRIVATE "${ORCA_SOURCE_DIR}/src"
    "${ORCA_SOURCE_DIR}/src/slic3r/GUI")
target_compile_definitions(curve_editor_gui_tests PRIVATE wxDEBUG_LEVEL=0)
target_link_libraries(curve_editor_gui_tests PRIVATE curve_editor_catch wx::core wx::base Boost::headers)

if(APPLE)
    enable_language(OBJCXX)
    set_target_properties(curve_editor_gui_tests PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
    target_sources(curve_editor_gui_tests PRIVATE "${ORCA_SOURCE_DIR}/src/slic3r/GUI/CurveEditorPanelMac.mm")
    target_link_libraries(curve_editor_gui_tests PRIVATE "-framework AppKit")
elseif(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CURVE_GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
    target_link_libraries(curve_editor_gui_tests PRIVATE PkgConfig::CURVE_GTK)
endif()

enable_testing()
list(APPEND CMAKE_MODULE_PATH "${ORCA_SOURCE_DIR}/tests/catch2/extras")
include(Catch)
catch_discover_tests(curve_editor_gui_tests ADD_TAGS_AS_LABELS)

find_program(CURVE_XGETTEXT NAMES xgettext HINTS "${ORCA_SOURCE_DIR}/tools")
find_program(CURVE_MSGFMT NAMES msgfmt HINTS "${ORCA_SOURCE_DIR}/tools")
if(CURVE_XGETTEXT AND CURVE_MSGFMT)
    add_test(NAME curve_editor_localization
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_localization.py"
            --xgettext "${CURVE_XGETTEXT}" --msgfmt "${CURVE_MSGFMT}")
    set_tests_properties(curve_editor_localization PROPERTIES LABELS "Localization")
endif()
