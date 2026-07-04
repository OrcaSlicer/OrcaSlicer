get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(wxWidgets_CONFIG_OPTIONS "--toolkit=gtk${SLIC3R_GTK}")
    find_package(wxWidgets 3.3 REQUIRED COMPONENTS base core adv html gl aui net media webview)
else ()
    find_package(wxWidgets 3.3 CONFIG REQUIRED COMPONENTS html adv gl core base webview aui net media)
endif ()

if(wxWidgets_USE_FILE)
    include(${wxWidgets_USE_FILE})
endif()

add_executable(${_TEST_NAME}_tests
    slic3rGUI_test_main.cpp
    )

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r Catch2::Catch2WithMain ${wxWidgets_LIBRARIES})
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
