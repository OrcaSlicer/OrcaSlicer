# Phase 2.5.3 — stage the profile pack fixture under the build tree so tests
# can read it via TEST_FIXTURES_DIR/profile_pack/orca-test-pack/. No binary
# build artifact — just JSON files copied at configure time.

set(_PACK_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/orca-test-pack")
set(_PACK_DST  "${CMAKE_BINARY_DIR}/tests/engine/fixtures/profile_pack/orca-test-pack")

file(MAKE_DIRECTORY "${_PACK_DST}/vendor/OrcaTestVendor/machine")
configure_file("${_PACK_SRC}/manifest.json"                                        "${_PACK_DST}/manifest.json"                                        COPYONLY)
configure_file("${_PACK_SRC}/vendor/OrcaTestVendor.json"                           "${_PACK_DST}/vendor/OrcaTestVendor.json"                           COPYONLY)
configure_file("${_PACK_SRC}/vendor/OrcaTestVendor/machine/OrcaTest Cube 220.json" "${_PACK_DST}/vendor/OrcaTestVendor/machine/OrcaTest Cube 220.json" COPYONLY)
