// test_model_transforms.cpp
//
// Unit tests for Slic3r::SliceCore::apply_model_transforms().
//
// Fixture: reuses the 3mf fixture from test_slice_golden.cpp (tests/data/test_3mf/).
// If the fixture is absent (not all CI environments ship test data), each test
// skips gracefully via WARN + SUCCEED.
//
// Catch2 v3 (Catch2::Catch2WithMain).

#include <catch2/catch_all.hpp>

#include "ModelTransforms.hpp"
#include "SliceTypes.hpp"

#include "libslic3r/Model.hpp"             // Model, ModelObject, BoundingBoxf3
                                           // (pulls in bbs_3mf.hpp -> LoadStrategy,
                                           //  PlateDataPtrs, Semver)
#include "libslic3r/PrintConfig.hpp"      // DynamicPrintConfig, ConfigSubstitutionContext
#include "libslic3r/Utils.hpp"            // CLI_INVALID_PARAMS

#include <boost/filesystem.hpp>
#include <string>

using namespace Slic3r;
using namespace Slic3r::SliceCore;

namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// Helpers  (mirrors fixture-locating pattern from test_slice_golden.cpp)
// ---------------------------------------------------------------------------

namespace {

std::string fixture_3mf()
{
    const fs::path test_data(TEST_DATA_DIR);
    return (test_data / "test_3mf" / "Ger\xC3\xA4te" / "B\xC3\xBC" "chse.3mf").string();
}

// Load the 3MF fixture into a Model.  Returns false (and leaves model default-
// constructed) if the file is not present.
bool load_fixture(Model &model)
{
    const std::string path = fixture_3mf();
    if (!fs::exists(path))
        return false;

    DynamicPrintConfig          config;
    ConfigSubstitutionContext   subs(ForwardCompatibilitySubstitutionRule::Enable);
    PlateDataPtrs               plate_data;
    std::vector<Preset *>       project_presets;
    bool                        is_bbl = false;  // is_xxx param (bool*)
    Semver                      version;

    // LoadStrategy confirmed in libslic3r/Format/bbs_3mf.hpp:162
    const LoadStrategy strategy =
        LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances;

    try {
        // Signature: read_from_file(path, config*, subs*, strategy, plate_data*,
        //                           project_presets*, is_xxx*, version*, proFn,
        //                           stlFn, project*, plate_id, objFn)
        // Confirmed: libslic3r/Model.hpp:1598
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
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("apply_model_transforms: scale > 0 succeeds",
          "[SliceCore][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping scale test");
        SUCCEED();
        return;
    }

    Transforms t;
    t.scale = 2.0;

    std::string err;
    REQUIRE(apply_model_transforms(model, t, err));
    CHECK(err.empty());
}

TEST_CASE("apply_model_transforms: scale <= 0 returns false",
          "[SliceCore][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping scale<=0 test");
        SUCCEED();
        return;
    }

    std::string err;

    SECTION("scale == 0") {
        Transforms t;
        t.scale = 0.0;
        const bool ok = apply_model_transforms(model, t, err);
        CHECK_FALSE(ok);
        CHECK_FALSE(err.empty());
    }

    SECTION("scale < 0") {
        Transforms t;
        t.scale = -1.5;
        const bool ok = apply_model_transforms(model, t, err);
        CHECK_FALSE(ok);
        CHECK_FALSE(err.empty());
    }
}

TEST_CASE("apply_model_transforms: rotate does not throw",
          "[SliceCore][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping rotate test");
        SUCCEED();
        return;
    }

    Transforms t;
    t.rotate   = 45.0;   // Z
    t.rotate_x = 10.0;
    t.rotate_y = -5.0;

    std::string err;
    REQUIRE_NOTHROW(apply_model_transforms(model, t, err));
    CHECK(err.empty());
}

TEST_CASE("apply_model_transforms: ensure_on_bed lifts min-z to ~0",
          "[SliceCore][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping ensure_on_bed test");
        SUCCEED();
        return;
    }

    // Sink the first object far below the bed to create a test condition.
    if (!model.objects.empty())
        model.objects.front()->translate(0.0, 0.0, -50.0);

    Transforms t;
    t.ensure_on_bed = true;

    std::string err;
    REQUIRE(apply_model_transforms(model, t, err));

    // After ensure_on_bed, every object's min-Z bounding box should be >= -epsilon.
    for (const ModelObject *o : model.objects) {
        const BoundingBoxf3 bb = o->bounding_box_exact();
        REQUIRE_THAT(bb.min.z(), Catch::Matchers::WithinAbs(0.0, 1.0));
    }
}

TEST_CASE("apply_model_transforms: convert_unit is a no-op on a normal mm model",
          "[SliceCore][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping convert_unit test");
        SUCCEED();
        return;
    }

    // Record bounding box before.
    BoundingBoxf3 bb_before;
    for (const ModelObject *o : model.objects)
        bb_before.merge(o->bounding_box_exact());

    Transforms t;
    t.convert_unit = true;   // The fixture is already in mm — should be a no-op.

    std::string err;
    REQUIRE(apply_model_transforms(model, t, err));

    BoundingBoxf3 bb_after;
    for (const ModelObject *o : model.objects)
        bb_after.merge(o->bounding_box_exact());

    // Dimensions should be unchanged (within floating-point noise).
    const Vec3d size_before = bb_before.size();
    const Vec3d size_after  = bb_after.size();
    REQUIRE_THAT(size_after.x(), Catch::Matchers::WithinRel(size_before.x(), 0.001));
    REQUIRE_THAT(size_after.y(), Catch::Matchers::WithinRel(size_before.y(), 0.001));
    REQUIRE_THAT(size_after.z(), Catch::Matchers::WithinRel(size_before.z(), 0.001));
}
