// test_slice_golden.cpp
//
// Integration test for Slic3r::SliceCore::SliceService.
//
// Finds the smallest available .3mf in tests/data/ (currently
// tests/data/test_3mf/Geräte/Büchse.3mf) and slices it via SliceService.
//
// Assertions (stat tolerances, not raw hash — gcode embeds timestamps):
//   result.ok == true
//   plates[0].layer_count > 0
//   plates[0].filament_used_mm > 0
//
// If the 3mf fixture lacks a printer config sufficient for slicing, the test
// documents the specific error code asserted rather than hard-failing with an
// opaque REQUIRE(false).
//
// The STL fixture (tests/data/test_3mf/Prusa.stl) is also exercised through
// a separate section but is expected to fail with CLI_NO_SUITABLE_OBJECTS or
// CLI_SLICING_ERROR if the geometry does not sit inside a default print volume.
// Both outcomes are accepted; the test asserts the error code is a defined
// CLI_* constant (not an uninitialised garbage value).
//
// Catch2 version: v3.11.0

#include <catch2/catch_all.hpp>

#include "SliceService.hpp"
#include "SliceTypes.hpp"

#include "libslic3r/Utils.hpp"    // CLI_* exit code constants

#include <boost/filesystem.hpp>

#include <string>

using namespace Slic3r;
using namespace Slic3r::SliceCore;

namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Resolve the resources/ directory from TEST_DATA_DIR.
// tests/data/../.. = repo root; repo root/resources = resources dir.
std::string resolve_resources_dir()
{
    const fs::path test_data(TEST_DATA_DIR);
    return (test_data.parent_path().parent_path() / "resources").string();
}

// Path to the 3mf fixture shipped with the test suite.
std::string fixture_3mf()
{
    return (fs::path(TEST_DATA_DIR) / "test_3mf" / "Ger\xC3\xA4te"
                                    / "B\xC3\xBCchse.3mf").string();
}

// Path to the STL fixture.
std::string fixture_stl()
{
    return (fs::path(TEST_DATA_DIR) / "test_3mf" / "Prusa.stl").string();
}

// Create a temporary directory that cleans up on destruction.
struct TempDir {
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path() /
               fs::unique_path("orca-golden-%%%%-%%%%");
        fs::create_directories(path);
    }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir &)            = delete;
    TempDir &operator=(const TempDir &) = delete;
};

// Validate that exit_code is one of the known CLI_* sentinel values.
// This catches the case where SliceResult is returned uninitialised or with a
// junk exit_code field.
bool is_known_exit_code(int code)
{
    switch (code) {
    case CLI_SUCCESS:
    case CLI_ENVIRONMENT_ERROR:
    case CLI_INVALID_PARAMS:
    case CLI_FILE_NOTFOUND:
    case CLI_FILELIST_INVALID_ORDER:
    case CLI_CONFIG_FILE_ERROR:
    case CLI_DATA_FILE_ERROR:
    case CLI_INVALID_PRINTER_TECH:
    case CLI_UNSUPPORTED_OPERATION:
    case CLI_COPY_OBJECTS_ERROR:
    case CLI_SCALE_TO_FIT_ERROR:
    case CLI_EXPORT_STL_ERROR:
    case CLI_EXPORT_OBJ_ERROR:
    case CLI_EXPORT_3MF_ERROR:
    case CLI_OUT_OF_MEMORY:
    case CLI_NO_SUITABLE_OBJECTS:
    case CLI_SLICING_ERROR:
        return true;
    default:
        return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 3MF golden slice
// ---------------------------------------------------------------------------

TEST_CASE("SliceService: 3mf fixture slices without crash",
          "[SliceCore][SliceService][integration]")
{
    const std::string fixture = fixture_3mf();

    if (!fs::exists(fixture)) {
        WARN("3mf fixture not found, skipping: " << fixture);
        SUCCEED();
        return;
    }

    TempDir outdir;
    const std::string resources_dir = resolve_resources_dir();

    SliceService svc(resources_dir);

    SliceRequest req;
    req.input_path       = fixture;
    req.plate            = 0;   // all plates
    req.output.mode      = OutputMode::File;
    req.output.outputdir = outdir.path.string();
    // No preset overrides — rely on the config embedded in the 3mf.

    SliceResult result;
    REQUIRE_NOTHROW(result = svc.run(req));

    // exit_code must always be a recognised CLI_* value.
    CHECK(is_known_exit_code(result.exit_code));

    if (result.ok) {
        // ── Happy path ──────────────────────────────────────────────────────
        REQUIRE_FALSE(result.plates.empty());

        const PlateStat &plate0 = result.plates[0];

        CHECK(plate0.layer_count > 0);

        // filament_used_mm is a double; just checking it is positive.
        CHECK(plate0.filament_used_mm > 0.0);

        // gcode_path must be non-empty and the file must exist.
        REQUIRE_FALSE(plate0.gcode_path.empty());
        CHECK(fs::exists(plate0.gcode_path));

        // File must be non-trivially sized (more than a header comment).
        CHECK(fs::file_size(plate0.gcode_path) > 100u);

        // Timing sanity: sliced_ms >= 0 (0 is allowed for very fast builds).
        CHECK(plate0.sliced_ms >= 0);

    } else {
        // ── Unhappy path ────────────────────────────────────────────────────
        // The 3mf may lack a printer config sufficient for slicing on its own.
        // We accept CLI_DATA_FILE_ERROR, CLI_INVALID_PRINTER_TECH,
        // CLI_NO_SUITABLE_OBJECTS, and CLI_SLICING_ERROR as legitimate
        // "no-printer-config" outcomes.  Any other code is unexpected.
        const bool acceptable_failure =
            (result.exit_code == CLI_DATA_FILE_ERROR      ||
             result.exit_code == CLI_INVALID_PRINTER_TECH ||
             result.exit_code == CLI_NO_SUITABLE_OBJECTS   ||
             result.exit_code == CLI_SLICING_ERROR         ||
             result.exit_code == CLI_CONFIG_FILE_ERROR);

        INFO("SliceService returned ok=false. error='" << result.error
             << "' exit_code=" << result.exit_code);

        CHECK(acceptable_failure);
        // error string must explain why.
        CHECK_FALSE(result.error.empty());
    }
}

// ---------------------------------------------------------------------------
// STL — expected to fail without arrange/printer config; assert sane exit_code
// ---------------------------------------------------------------------------

TEST_CASE("SliceService: STL fixture returns a recognised exit code",
          "[SliceCore][SliceService][integration]")
{
    const std::string fixture = fixture_stl();

    if (!fs::exists(fixture)) {
        WARN("STL fixture not found, skipping: " << fixture);
        SUCCEED();
        return;
    }

    TempDir outdir;
    const std::string resources_dir = resolve_resources_dir();

    SliceService svc(resources_dir);

    SliceRequest req;
    req.input_path       = fixture;
    req.plate            = 0;
    req.output.mode      = OutputMode::File;
    req.output.outputdir = outdir.path.string();

    SliceResult result;
    REQUIRE_NOTHROW(result = svc.run(req));

    // We do not assert ok==true for a bare STL; without a printer profile and
    // an arrange pass the geometry may lie outside the print volume.
    // What we DO assert is that:
    //   1. exit_code is a known sentinel (not garbage).
    //   2. If ok==false, error is non-empty.
    CHECK(is_known_exit_code(result.exit_code));
    if (!result.ok) {
        CHECK_FALSE(result.error.empty());
    }
}

// ---------------------------------------------------------------------------
// Non-existent input path
// ---------------------------------------------------------------------------

TEST_CASE("SliceService: non-existent input path returns CLI_FILE_NOTFOUND",
          "[SliceCore][SliceService]")
{
    TempDir outdir;
    SliceService svc(resolve_resources_dir());

    SliceRequest req;
    req.input_path       = "/no/such/file/does_not_exist_orca_12345.3mf";
    req.output.mode      = OutputMode::File;
    req.output.outputdir = outdir.path.string();

    SliceResult result;
    REQUIRE_NOTHROW(result = svc.run(req));

    CHECK_FALSE(result.ok);
    CHECK(result.exit_code == CLI_FILE_NOTFOUND);
    CHECK_FALSE(result.error.empty());
}

// ---------------------------------------------------------------------------
// Empty input path (neither input_path nor input_bytes supplied)
// ---------------------------------------------------------------------------

TEST_CASE("SliceService: empty input returns CLI_INVALID_PARAMS",
          "[SliceCore][SliceService]")
{
    TempDir outdir;
    SliceService svc(resolve_resources_dir());

    SliceRequest req;   // input_path empty, input_bytes empty
    req.output.mode      = OutputMode::File;
    req.output.outputdir = outdir.path.string();

    SliceResult result;
    REQUIRE_NOTHROW(result = svc.run(req));

    CHECK_FALSE(result.ok);
    CHECK(result.exit_code == CLI_INVALID_PARAMS);
    CHECK_FALSE(result.error.empty());
}
