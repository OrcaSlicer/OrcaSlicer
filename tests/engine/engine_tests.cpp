// Catch2 main translation unit for the orca engine test suite.
//
// `Catch2::Catch2WithMain` (linked in tests/engine/CMakeLists.txt) provides the
// `main()` entry point — this file just pulls in the Catch2 header so the suite
// has at least one TU including <catch2/catch_all.hpp> at top level, matching
// the pattern used by tests/libslic3r/libslic3r_tests.cpp.
#include <catch2/catch_all.hpp>
