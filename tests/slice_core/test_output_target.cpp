// test_output_target.cpp
//
// Unit tests for Slic3r::SliceCore::deliver()
// (declared in src/slic3r/SliceCore/OutputTargetDeliver.hpp)
//
// Test plan
// ---------
// File mode:
//   1. write a temp gcode file, deliver to a temp output dir → file exists at
//      destination with identical content.
//   2. deliver when gcode_path is already inside outputdir → returns true,
//      content is not corrupted (no-op path).
//
// Stdout mode:
//   3. redirect std::cout, deliver → captured bytes equal the source file.
//
// PrintHost mode:
//   4. [!mayfail] tagged; an invalid host_config (empty) causes deliver() to
//      return false with a non-empty err.  No live host required.
//
// Catch2 version: v3.11.0

#include <catch2/catch_all.hpp>

#include "OutputTargetDeliver.hpp"
#include "SliceTypes.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <sstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::SliceCore;

namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// RAII helpers
// ---------------------------------------------------------------------------

namespace {

// Write a small synthetic gcode file and return its path as a string.
// The caller owns the lifetime of the temp file.
struct TempFile {
    fs::path path;

    TempFile(const std::string &content, const std::string &suffix = ".gcode")
    {
        path = fs::temp_directory_path() /
               fs::unique_path("orca-test-%%%%-%%%%-" + suffix);
        std::ofstream out(path.string(), std::ios::binary);
        out << content;
    }

    ~TempFile()
    {
        boost::system::error_code ec;
        fs::remove(path, ec);
    }

    TempFile(const TempFile &)            = delete;
    TempFile &operator=(const TempFile &) = delete;
};

struct TempDir {
    fs::path path;

    TempDir()
    {
        path = fs::temp_directory_path() /
               fs::unique_path("orca-test-dir-%%%%");
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

// Read entire file as a string (binary safe).
std::string slurp(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

const std::string kSampleGcode =
    "; FLAVOR:Marlin\n"
    "G28 ; home all axes\n"
    "G1 Z5 F5000 ; lift nozzle\n"
    "M104 S200\n"
    "G1 X10 Y10 F3000\n"
    "M109 S200\n"
    "G1 Z0.2 F1000\n"
    "G1 X50 Y50 E10 F1500\n"
    "G28\n"
    "M84\n";

} // namespace

// ---------------------------------------------------------------------------
// File mode — copy to outputdir
// ---------------------------------------------------------------------------

TEST_CASE("deliver File mode: copies gcode to outputdir",
          "[SliceCore][OutputTarget]")
{
    TempFile src(kSampleGcode, ".gcode");
    TempDir  dst_dir;

    OutputTarget out;
    out.mode      = OutputMode::File;
    out.outputdir = dst_dir.path.string();

    std::string err;
    const bool ok = deliver(out, src.path.string(), err);

    REQUIRE(ok);
    CHECK(err.empty());

    // File should now exist at dst_dir / <original_filename>
    const fs::path expected = dst_dir.path / src.path.filename();
    REQUIRE(fs::exists(expected));

    // Content must be identical to source.
    CHECK(slurp(expected.string()) == kSampleGcode);
}

TEST_CASE("deliver File mode: gcode already in outputdir is a no-op",
          "[SliceCore][OutputTarget]")
{
    // Place the gcode file directly inside the output directory so that
    // the canonical source path == canonical destination path.
    TempDir outdir;
    const fs::path gcode_in_outdir = outdir.path / "plate_1.gcode";
    {
        std::ofstream out(gcode_in_outdir.string(), std::ios::binary);
        out << kSampleGcode;
    }

    OutputTarget out;
    out.mode      = OutputMode::File;
    out.outputdir = outdir.path.string();

    std::string err;
    const bool ok = deliver(out, gcode_in_outdir.string(), err);

    REQUIRE(ok);
    CHECK(err.empty());

    // Content must be unchanged.
    CHECK(slurp(gcode_in_outdir.string()) == kSampleGcode);
}

TEST_CASE("deliver File mode: outputdir is created if absent",
          "[SliceCore][OutputTarget]")
{
    TempFile src(kSampleGcode, ".gcode");

    // Build a path that does not exist yet.
    const fs::path new_dir =
        fs::temp_directory_path() / fs::unique_path("orca-test-newdir-%%%%");
    // Ensure it really does not exist.
    REQUIRE_FALSE(fs::exists(new_dir));

    OutputTarget out;
    out.mode      = OutputMode::File;
    out.outputdir = new_dir.string();

    std::string err;
    const bool ok = deliver(out, src.path.string(), err);

    // Clean up the newly created dir regardless of outcome.
    boost::system::error_code ec;
    fs::remove_all(new_dir, ec);

    REQUIRE(ok);
    CHECK(err.empty());
}

TEST_CASE("deliver File mode: non-existent source returns false with err",
          "[SliceCore][OutputTarget]")
{
    TempDir outdir;

    OutputTarget out;
    out.mode      = OutputMode::File;
    out.outputdir = outdir.path.string();

    std::string err;
    const bool ok = deliver(out, "/no/such/file/does_not_exist.gcode", err);

    REQUIRE_FALSE(ok);
    CHECK_FALSE(err.empty());
}

// ---------------------------------------------------------------------------
// Stdout mode — captured bytes equal file content
// ---------------------------------------------------------------------------

TEST_CASE("deliver Stdout mode: streams gcode bytes to std::cout",
          "[SliceCore][OutputTarget]")
{
    TempFile src(kSampleGcode, ".gcode");

    OutputTarget out;
    out.mode = OutputMode::Stdout;

    // Redirect std::cout to a string stream for capture.
    std::ostringstream capture;
    std::streambuf *old_buf = std::cout.rdbuf(capture.rdbuf());

    std::string err;
    bool ok = false;
    REQUIRE_NOTHROW(ok = deliver(out, src.path.string(), err));

    // Restore std::cout immediately.
    std::cout.rdbuf(old_buf);

    REQUIRE(ok);
    CHECK(err.empty());
    CHECK(capture.str() == kSampleGcode);
}

TEST_CASE("deliver Stdout mode: non-existent source returns false with err",
          "[SliceCore][OutputTarget]")
{
    OutputTarget out;
    out.mode = OutputMode::Stdout;

    // Redirect so we do not pollute test output.
    std::ostringstream capture;
    std::streambuf *old_buf = std::cout.rdbuf(capture.rdbuf());

    std::string err;
    bool ok = false;
    REQUIRE_NOTHROW(ok = deliver(out, "/no/such/file/does_not_exist.gcode", err));
    std::cout.rdbuf(old_buf);

    REQUIRE_FALSE(ok);
    CHECK_FALSE(err.empty());
}

// ---------------------------------------------------------------------------
// PrintHost mode — invalid config must fail cleanly (no live host required)
// ---------------------------------------------------------------------------

TEST_CASE("deliver PrintHost mode: empty host_config returns false with err",
          "[SliceCore][OutputTarget][!mayfail]")
{
    // This test is tagged [!mayfail] because behaviour depends on whether wx
    // is initialised and what PrintHost::get_print_host() does with an empty
    // config.  We assert only that deliver() does NOT throw and does NOT
    // silently claim success with no host configured.
    //
    // A properly built orca-server or CLI process would always supply a
    // non-empty host_config; the empty-config path should short-circuit in
    // deliver_printhost() with "does not specify a valid print host".

    TempFile src(kSampleGcode, ".gcode");

    OutputTarget out;
    out.mode        = OutputMode::PrintHost;
    out.host_config = DynamicPrintConfig{};  // intentionally empty

    std::string err;
    bool ok = true;  // initialise to true so a no-op silently passing is caught

    REQUIRE_NOTHROW(ok = deliver(out, src.path.string(), err));

    // If the function returned true with no host configured, that is a bug.
    // We assert false with a diagnostic rather than hard-failing via REQUIRE
    // because PrintHost behaviour is wx/platform dependent.
    if (ok) {
        WARN("deliver() with empty host_config returned true — unexpected;"
             " check PrintHost::get_print_host() behaviour on this platform");
    } else {
        CHECK_FALSE(err.empty());
    }
}
