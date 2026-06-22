# Tiny fixture plugin used by tests/engine/test_plugin_loader.cpp (Phase 1.5.1).
# Builds as a SHARED library named "test.fixture.loader_plugin" so the
# PluginManager's per-platform extension switch (.so / .dll / .dylib) resolves
# to the right entry binary when the test runs.
#
# We deliberately do NOT link libslic3r here — this fixture only needs the
# engine's PUBLIC headers (orca/c_api.h + orca/plugin_api.h), which live in
# engine/include and are added as PUBLIC include dirs on the engine target.
# Including them by relative path keeps the fixture's link surface tiny.

add_library(test_fixture_loader_plugin SHARED plugin.c)

# Stable, config-agnostic output dir matching PluginManager's expected layout:
# <fixtures_root>/loader_plugin/{<id>.{so,dll,dylib}, manifest.json}.
# Without this the multi-config generators (Ninja Multi-Config, MSVC) drop the
# binary under a per-config subdir which the loader test can't predict at
# compile time. Forcing a flat staged dir is the cleanest fix.
set(_STAGED_DIR "${CMAKE_BINARY_DIR}/tests/engine/fixtures/loader_plugin")

set_target_properties(test_fixture_loader_plugin PROPERTIES
    OUTPUT_NAME       "test.fixture.loader_plugin"
    PREFIX            ""                # no "lib" prefix; PluginManager looks for <id>.so
    C_STANDARD        11
    C_VISIBILITY_PRESET hidden          # only ORCA_PLUGIN_EXPORT symbols are visible
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

target_include_directories(test_fixture_loader_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/include)

# Drop a copy of manifest.json next to the binary in the staged dir.
add_custom_command(TARGET test_fixture_loader_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
        ${_STAGED_DIR}/manifest.json)
