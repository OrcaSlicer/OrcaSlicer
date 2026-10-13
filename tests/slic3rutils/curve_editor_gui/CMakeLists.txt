cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(curve_editor_gui_checks LANGUAGES CXX)
    if(APPLE)
        enable_language(OBJCXX)
    endif()
    set(CURVE_STANDALONE TRUE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
get_filename_component(ORCA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
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
if(CURVE_STANDALONE)
    add_library(curve_editor_catch STATIC "${ORCA_SOURCE_DIR}/tests/catch2/extras/catch_amalgamated.cpp")
    target_include_directories(curve_editor_catch PUBLIC "${ORCA_SOURCE_DIR}/tests/catch2/extras")
else()
    add_library(curve_editor_catch INTERFACE)
    target_link_libraries(curve_editor_catch INTERFACE Catch2::Catch2WithMain)
    target_include_directories(curve_editor_catch INTERFACE "${ORCA_SOURCE_DIR}/tests/catch2/extras")
endif()
add_executable(curve_editor_gui_tests test_curve_editor.cpp "${_test_panel}"
    "${ORCA_SOURCE_DIR}/src/libslic3r/CurveModel.cpp"
    "${ORCA_SOURCE_DIR}/src/libslic3r/LocalesUtils.cpp"
    "${ORCA_SOURCE_DIR}/src/libslic3r/GCode/PchipInterpolatorHelper.cpp")
target_include_directories(curve_editor_gui_tests PRIVATE "${ORCA_SOURCE_DIR}/src"
    "${ORCA_SOURCE_DIR}/src/slic3r/GUI")
target_include_directories(curve_editor_gui_tests SYSTEM PRIVATE "${ORCA_SOURCE_DIR}/deps_src")
target_compile_definitions(curve_editor_gui_tests PRIVATE wxDEBUG_LEVEL=0)
target_link_libraries(curve_editor_gui_tests PRIVATE curve_editor_catch ${wxWidgets_LIBRARIES} Boost::headers)

if(APPLE)
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
# Pointer capture belongs to the display, so parallel processes must not steal it.
set(_curve_test_properties RESOURCE_LOCK curve_editor_display)
if(UNIX AND NOT APPLE)
    # Headless GTK checks do not use AT-SPI; an absent accessibility bus can stall startup.
    list(APPEND _curve_test_properties ENVIRONMENT NO_AT_BRIDGE=1)
endif()
catch_discover_tests(curve_editor_gui_tests ADD_TAGS_AS_LABELS
    PROPERTIES ${_curve_test_properties})

find_program(CURVE_XGETTEXT NAMES xgettext HINTS "${ORCA_SOURCE_DIR}/tools")
find_program(CURVE_MSGFMT NAMES msgfmt HINTS "${ORCA_SOURCE_DIR}/tools")
if(CURVE_XGETTEXT AND CURVE_MSGFMT)
    if(CURVE_STANDALONE)
        add_test(NAME curve_editor_localization
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_localization.py"
                --xgettext "${CURVE_XGETTEXT}" --msgfmt "${CURVE_MSGFMT}")
        set_tests_properties(curve_editor_localization PROPERTIES LABELS "Localization")
    else()
        # CI's runtime artifact has no source/catalog checkout or build-host Python.
        # Validate these contracts during its build, then package only native event tests.
        add_custom_target(curve_editor_localization_check ALL
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_localization.py"
                --xgettext "${CURVE_XGETTEXT}" --msgfmt "${CURVE_MSGFMT}"
            VERBATIM)
        # Flatpak builds explicit test targets instead of ALL.
        add_dependencies(curve_editor_gui_tests curve_editor_localization_check)
    endif()
endif()
