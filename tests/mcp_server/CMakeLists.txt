if(ENABLE_MCP_SERVER)
    get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

    add_executable(${_TEST_NAME}_tests
        test_jsonrpc.cpp
        test_mcp_protocol.cpp
        test_tool_schemas.cpp
    )

    target_link_libraries(${_TEST_NAME}_tests
        test_common
        libslic3r_gui
        libslic3r
        Catch2::Catch2WithMain
    )

    target_include_directories(${_TEST_NAME}_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/slic3r
    )

    set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

    if (WIN32)
        if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
            orcaslicer_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
        elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
            orcaslicer_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
        else()
            orcaslicer_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
        endif()
    endif()

    catch_discover_tests(${_TEST_NAME}_tests)
endif()
