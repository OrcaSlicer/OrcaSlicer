// test_setup.cpp
//
// Global test-process initialisation for the slice_core test suite.
//
// Model::read_from_file() triggers the BBS backup machinery, which calls
// temporary_dir() to locate the backup root.  When the global is empty
// (never initialised) the path collapses to an absolute "/orcaslicer_model"
// prefix — a read-only location on macOS and Linux CI runners.
//
// This Catch2 v3 event-listener runs before any TEST_CASE and populates
// the global with the OS temp directory, so every subsequent model load
// writes backups to a writable location on all platforms.

#include <catch2/catch_all.hpp>

#include "libslic3r/Utils.hpp"   // set_temporary_dir / temporary_dir

#include <boost/filesystem.hpp>

namespace {

struct SliceCoreTestSetup : Catch::EventListenerBase {
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const &) override
    {
        if (Slic3r::temporary_dir().empty()) {
            Slic3r::set_temporary_dir(
                boost::filesystem::temp_directory_path().string());
        }
    }
};

CATCH_REGISTER_LISTENER(SliceCoreTestSetup)

} // namespace
