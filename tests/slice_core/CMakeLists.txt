get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

# ---- Sources ----------------------------------------------------------------
# test_server_e2e.cpp requires JobQueue and Authenticator, which are compiled
# only into the orca-server *executable* (not a library) and only when
# SLIC3R_SERVER=ON.  We add those two translation units directly to this test
# executable so the tests link without requiring the full server build.
# The sources are guarded by the SLIC3R_SERVER option below so that when the
# server is disabled the server-side unit tests are simply omitted.

set(_CORE_SOURCES
    # Global process setup: initialises temporary_dir() before any model load
    # so the BBS backup path resolves to a writable OS temp location on all
    # platforms (avoids "Read-only file system" at /orcaslicer_model on macOS/Linux).
    test_setup.cpp
    test_preset_resolver.cpp
    test_output_target.cpp
    test_slice_golden.cpp
    test_model_transforms.cpp
    test_placement.cpp
    test_preview.cpp
)

if (SLIC3R_SERVER)
    list(APPEND _CORE_SOURCES
        # Compile server translation units directly into the test executable.
        # The Server directory is added to include paths below so that
        # #include "JobQueue.hpp" etc. resolve without a path prefix.
        ${CMAKE_SOURCE_DIR}/src/slic3r/Server/JobQueue.cpp
        ${CMAKE_SOURCE_DIR}/src/slic3r/Server/Authenticator.cpp
        ${CMAKE_SOURCE_DIR}/src/slic3r/Server/RequestMapping.cpp
        # test files
        test_server_e2e.cpp
        test_json_request.cpp
        # test_placement.cpp parse_objects half requires RequestMapping.cpp
        # (the apply_object_placements half in test_placement.cpp is already
        # compiled via _CORE_SOURCES above; the SLIC3R_SERVER guard inside the
        # file gates the parse_objects section at compile time).
    )
endif ()

add_executable(${_TEST_NAME}_tests
    ${_CORE_SOURCES}
)

# ---- Include paths ----------------------------------------------------------
target_include_directories(${_TEST_NAME}_tests PRIVATE
    # Gives #include "SliceTypes.hpp", "PresetResolver.hpp", etc.
    ${CMAKE_SOURCE_DIR}/src/slic3r/SliceCore
    # Gives #include "libslic3r/PrintConfig.hpp" etc. (mirroring libslic3r tests)
    ${CMAKE_SOURCE_DIR}/src
)

if (SLIC3R_SERVER)
    target_include_directories(${_TEST_NAME}_tests PRIVATE
        # Gives #include "JobQueue.hpp", "Authenticator.hpp" without path prefix
        ${CMAKE_SOURCE_DIR}/src/slic3r/Server
    )
endif ()

# ---- Link targets -----------------------------------------------------------
# liborca_slice_core pulls libslic3r + libslic3r_gui transitively.
# test_common supplies TEST_DATA_DIR and CATCH_CONFIG_FAST_COMPILE.
# Catch2::Catch2WithMain supplies the main() entry point and the v3 framework.
target_link_libraries(${_TEST_NAME}_tests
    test_common
    liborca_slice_core
    Catch2::Catch2WithMain
)

if (SLIC3R_SERVER)
    # Boost compiled libs (system, filesystem, thread, log) + Beast/Asio headers.
    # nlohmann is needed by JobQueue (UUID generation via Boost.Uuid uses it
    # only via boost_libs; nlohmann provides the vendored JSON header).
    target_link_libraries(${_TEST_NAME}_tests
        boost_libs
        nlohmann_json
    )
    if (WIN32)
        target_link_libraries(${_TEST_NAME}_tests ws2_32.lib)
    endif ()
endif ()

# ---- Compile definitions ----------------------------------------------------
if (SLIC3R_SERVER)
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE SLIC3R_SERVER=1)
endif ()

# ---- CTest integration ------------------------------------------------------
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls()
catch_discover_tests(${_TEST_NAME}_tests ADD_TAGS_AS_LABELS)
