# Phase 3.4.1 — native runtime smoke fixture.
# Built as a SHARED library named "test.fixture.native_noop" so the
# PluginManager loader resolves it via the unversioned <id><platform_ext>
# fallback (Phase 3.1). Mirrors tests/engine/fixtures/loader_plugin/.

add_library(test_fixture_native_noop SHARED plugin.c)

set(_STAGED_DIR "${CMAKE_BINARY_DIR}/tests/engine/fixtures/native_noop")

set_target_properties(test_fixture_native_noop PROPERTIES
    OUTPUT_NAME       "test.fixture.native_noop"
    PREFIX            ""
    C_STANDARD        11
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY                ${_STAGED_DIR}
    LIBRARY_OUTPUT_DIRECTORY_DEBUG          ${_STAGED_DIR}
    LIBRARY_OUTPUT_DIRECTORY_RELEASE        ${_STAGED_DIR}
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${_STAGED_DIR}
    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL     ${_STAGED_DIR}
    RUNTIME_OUTPUT_DIRECTORY                ${_STAGED_DIR}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          ${_STAGED_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        ${_STAGED_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${_STAGED_DIR}
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     ${_STAGED_DIR})

target_include_directories(test_fixture_native_noop PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/include)

add_custom_command(TARGET test_fixture_native_noop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
        ${_STAGED_DIR}/manifest.json)
