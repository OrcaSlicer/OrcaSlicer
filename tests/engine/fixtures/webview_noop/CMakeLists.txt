# Phase 3.4.3 — webview runtime smoke fixture.
# No code to build; just stage manifest.json + index.html under the
# canonical TEST_FIXTURES_DIR so test_runtime_fixtures.cpp can find them.

set(_STAGED_DIR "${CMAKE_BINARY_DIR}/tests/engine/fixtures/webview_noop")
file(MAKE_DIRECTORY "${_STAGED_DIR}")

# A custom target so engine_tests can depend on the staging step.
add_custom_target(test_fixture_webview_noop ALL
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json ${_STAGED_DIR}/manifest.json
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/index.html ${_STAGED_DIR}/index.html
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
        ${CMAKE_CURRENT_SOURCE_DIR}/index.html
    COMMENT "Staging webview_noop fixture")
