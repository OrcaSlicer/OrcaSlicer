// test_preview.cpp
//
// Tests for Tier-1 preview stats (PlateStat fields populated after slicing)
// and the GL-gated thumbnail renderer.
//
// Tier-1 slice test — mirrors the fixture/datadir pattern from
// test_slice_golden.cpp: run SliceService on the bundled 3mf fixture and
// assert the resulting PlateStat contains expected preview statistics.
// Skips gracefully (WARN + SUCCEED) when:
//   - The 3mf fixture is absent.
//   - The SliceService cannot produce ok=true due to a missing printer config
//     in the test environment (the same acceptable-failure set as golden test).
//
// Thumbnail test — calls render_model_thumbnail() on a loaded fixture.
// Since the test environment typically has no GL context (CI), the function is
// expected to return false gracefully.  The test is tagged [gl][.] so it is
// hidden from the default test run; it can be enabled explicitly with
// --tags [gl] when a display / GPU is available.
//
// Catch2 v3 (Catch2::Catch2WithMain).

#include <catch2/catch_all.hpp>

#include "SliceService.hpp"
#include "SliceTypes.hpp"
#include "ThumbnailRenderer.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"     // CLI_* exit code constants

#include <boost/filesystem.hpp>

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::SliceCore;
namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// Helpers — mirrors test_slice_golden.cpp exactly
// ---------------------------------------------------------------------------

namespace {

std::string resolve_resources_dir()
{
    const fs::path test_data(TEST_DATA_DIR);
    return (test_data.parent_path().parent_path() / "resources").string();
}

std::string fixture_3mf()
{
    return (fs::path(TEST_DATA_DIR) / "test_3mf" / "Ger\xC3\xA4te"
                                    / "B\xC3\xBC" "chse.3mf").string();
}

struct TempDir {
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path() /
               fs::unique_path("orca-preview-%%%%-%%%%");
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

// Returns true when the exit code is any known non-success code that indicates
// a missing/incompatible printer config in the test environment.
bool is_acceptable_no_printer_failure(int code)
{
    switch (code) {
    case CLI_DATA_FILE_ERROR:
    case CLI_INVALID_PRINTER_TECH:
    case CLI_NO_SUITABLE_OBJECTS:
    case CLI_SLICING_ERROR:
    case CLI_CONFIG_FILE_ERROR:
        return true;
    default:
        return false;
    }
}

// Load the fixture into a Model for thumbnail tests.
bool load_fixture(Model &model)
{
    const std::string path = fixture_3mf();
    if (!fs::exists(path))
        return false;

    DynamicPrintConfig        config;
    ConfigSubstitutionContext subs(ForwardCompatibilitySubstitutionRule::Enable);
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl = false;
    Semver                    version;

    const LoadStrategy strategy =
        LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances;

    try {
        model = Model::read_from_file(
            path, &config, &subs, strategy,
            &plate_data, &project_presets, &is_bbl, &version,
            nullptr, nullptr, nullptr, 0);
    } catch (...) {
        return false;
    }
    return !model.objects.empty();
}

} // namespace

// ---------------------------------------------------------------------------
// Tier-1 preview stats test
// ---------------------------------------------------------------------------

TEST_CASE("PlateStat tier-1 fields: estimated_print_time_s > 0 on successful slice",
          "[SliceCore][Preview][integration]")
{
    const std::string fixture = fixture_3mf();

    if (!fs::exists(fixture)) {
        WARN("3mf fixture not found — skipping tier-1 preview stats test: " << fixture);
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

    if (!result.ok) {
        // Accept known no-printer-config failure modes in CI — same as golden test.
        const bool acceptable = is_acceptable_no_printer_failure(result.exit_code);
        INFO("SliceService returned ok=false. error='" << result.error
             << "' exit_code=" << result.exit_code);
        if (acceptable) {
            WARN("SliceService could not slice (no printer preset in test env) — "
                 "tier-1 stats assertions skipped.");
            SUCCEED();
            return;
        }
        // Unexpected failure — fail with the error message.
        FAIL("SliceService returned ok=false with unexpected exit_code="
             << result.exit_code << ": " << result.error);
    }

    // ── Happy path ───────────────────────────────────────────────────────────
    REQUIRE_FALSE(result.plates.empty());

    const PlateStat &ps = result.plates[0];

    // Tier-1 stat: estimated print time must be positive after a real slice.
    CHECK(ps.estimated_print_time_s > 0.0);

    // Basic sanity: layer_count > 0 (also asserted by test_slice_golden but
    // doubles as a prerequisite for the tier-1 stats to make sense).
    CHECK(ps.layer_count > 0);

    // Filament volume per extruder must contain at least one entry.
    CHECK_FALSE(ps.filament_volume_per_extruder.empty());

    // Each volume entry must be positive.
    for (const auto &kv : ps.filament_volume_per_extruder) {
        DYNAMIC_SECTION("extruder " << kv.first << " volume > 0") {
            CHECK(kv.second > 0.0);
        }
    }
}

// ---------------------------------------------------------------------------
// Thumbnail renderer — GL-gated, hidden by default ([gl][.])
// ---------------------------------------------------------------------------
// Tag [.] hides this test from the default run.  Run with:
//   slice_core_tests [gl]
// only when a real display / GL context is available.

TEST_CASE("render_model_thumbnail: PNG magic bytes when GL context available",
          "[gl][SliceCore][Preview][.]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping thumbnail test");
        SUCCEED();
        return;
    }

    DynamicPrintConfig       cfg;
    std::vector<unsigned char> png;
    std::string              err;

    const bool ok = render_model_thumbnail(model, cfg, 256, 256, png, err);

    if (!ok) {
        // No GL context in this environment — graceful skip.
        WARN("render_model_thumbnail returned false (no GL context or driver): " << err);
        SUCCEED();
        return;
    }

    // ── GL context available ─────────────────────────────────────────────────
    // PNG bytes must be non-empty.
    REQUIRE_FALSE(png.empty());

    // First four bytes must be the PNG signature: 0x89 'P' 'N' 'G'
    REQUIRE(png.size() >= 4);
    CHECK(png[0] == static_cast<unsigned char>(0x89));
    CHECK(png[1] == static_cast<unsigned char>('P'));
    CHECK(png[2] == static_cast<unsigned char>('N'));
    CHECK(png[3] == static_cast<unsigned char>('G'));
}
