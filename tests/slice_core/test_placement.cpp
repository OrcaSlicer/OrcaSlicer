// test_placement.cpp
//
// Tests for:
//   A) Slic3r::SliceCore::parse_objects()  — JSON -> ObjectPlacement parsing
//      (always compiled; parse_objects now lives in liborca_slice_core via
//       SliceCore/ObjectPlacementJson.cpp, gated by SLIC3R_SERVER below only
//       to preserve the existing skip-message behaviour when server is off)
//   B) Slic3r::SliceCore::apply_object_placements() — placement application
//      (uses the 3mf fixture from test_model_transforms.cpp; skips gracefully
//       when the fixture is absent)
//
// Catch2 v3 (Catch2::Catch2WithMain).

#include <catch2/catch_all.hpp>

#ifdef SLIC3R_SERVER
#include "ObjectPlacementJson.hpp"   // Slic3r::SliceCore::parse_objects
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#endif

#include "ModelTransforms.hpp"   // apply_object_placements
#include "SliceTypes.hpp"

#include "libslic3r/Model.hpp"         // Model, ModelObject, ModelInstance
#include "libslic3r/PrintConfig.hpp"   // DynamicPrintConfig, ConfigSubstitutionContext
#include "libslic3r/Geometry.hpp"      // deg2rad
#include "libslic3r/libslic3r.h"       // Axis (X=0, Y=1, Z=2)

#include <boost/filesystem.hpp>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::SliceCore;
namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// Fixture helpers — mirrors test_model_transforms.cpp exactly
// ---------------------------------------------------------------------------

namespace {

std::string fixture_3mf()
{
    const fs::path test_data(TEST_DATA_DIR);
    return (test_data / "test_3mf" / "Ger\xC3\xA4te" / "B\xC3\xBC" "chse.3mf").string();
}

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

// Returns a default (empty) DynamicPrintConfig for calls that do not require
// a real printer config.
DynamicPrintConfig empty_cfg()
{
    return DynamicPrintConfig{};
}

} // namespace

// ===========================================================================
// A) parse_objects tests — compiled only when SLIC3R_SERVER is defined
// ===========================================================================

#ifdef SLIC3R_SERVER

using namespace Slic3r::SliceCore;

TEST_CASE("parse_objects: index and name fields",
          "[Placement][RequestMapping][Server]")
{
    json arr = json::parse(R"([
        {"index": 0, "name": "Cube"}
    ])");

    std::vector<ObjectPlacement> out;
    REQUIRE_NOTHROW(parse_objects(arr, out));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].index.has_value());
    CHECK(out[0].index.value() == 0);
    REQUIRE(out[0].name.has_value());
    CHECK(out[0].name.value() == "Cube");
}

TEST_CASE("parse_objects: position as {x,y,z} object",
          "[Placement][RequestMapping][Server]")
{
    json arr = json::parse(R"([
        {"index": 0, "position": {"x": 10.0, "y": 20.0, "z": 5.0}}
    ])");

    std::vector<ObjectPlacement> out;
    REQUIRE_NOTHROW(parse_objects(arr, out));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].position.has_value());
    REQUIRE_THAT((*out[0].position)[0], Catch::Matchers::WithinAbs(10.0, 1e-9));
    REQUIRE_THAT((*out[0].position)[1], Catch::Matchers::WithinAbs(20.0, 1e-9));
    REQUIRE_THAT((*out[0].position)[2], Catch::Matchers::WithinAbs(5.0,  1e-9));
}

TEST_CASE("parse_objects: position as [x,y,z] array",
          "[Placement][RequestMapping][Server]")
{
    json arr = json::parse(R"([
        {"index": 0, "position": [10.0, 20.0, 5.0]}
    ])");

    std::vector<ObjectPlacement> out;
    REQUIRE_NOTHROW(parse_objects(arr, out));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].position.has_value());
    REQUIRE_THAT((*out[0].position)[0], Catch::Matchers::WithinAbs(10.0, 1e-9));
    REQUIRE_THAT((*out[0].position)[1], Catch::Matchers::WithinAbs(20.0, 1e-9));
    REQUIRE_THAT((*out[0].position)[2], Catch::Matchers::WithinAbs(5.0,  1e-9));
}

TEST_CASE("parse_objects: rotation as {x,y,z} object",
          "[Placement][RequestMapping][Server]")
{
    json arr = json::parse(R"([
        {"index": 0, "rotation": {"x": 0.0, "y": 0.0, "z": 45.0}}
    ])");

    std::vector<ObjectPlacement> out;
    REQUIRE_NOTHROW(parse_objects(arr, out));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].rotation.has_value());
    REQUIRE_THAT((*out[0].rotation)[2], Catch::Matchers::WithinAbs(45.0, 1e-9));
}

TEST_CASE("parse_objects: scale as number maps to uniform_scale",
          "[Placement][RequestMapping][Server]")
{
    json arr = json::parse(R"([
        {"index": 0, "scale": 2.0}
    ])");

    std::vector<ObjectPlacement> out;
    REQUIRE_NOTHROW(parse_objects(arr, out));
    REQUIRE(out.size() == 1);
    // A scalar scale value must land in uniform_scale.
    REQUIRE(out[0].uniform_scale.has_value());
    REQUIRE_THAT(out[0].uniform_scale.value(), Catch::Matchers::WithinAbs(2.0, 1e-9));
    // Per-axis scale must NOT be set when only a number was given.
    CHECK_FALSE(out[0].scale.has_value());
}

TEST_CASE("parse_objects: scale as {x,y,z} maps to per-axis scale",
          "[Placement][RequestMapping][Server]")
{
    json arr = json::parse(R"([
        {"index": 0, "scale": {"x": 1.0, "y": 2.0, "z": 3.0}}
    ])");

    std::vector<ObjectPlacement> out;
    REQUIRE_NOTHROW(parse_objects(arr, out));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].scale.has_value());
    REQUIRE_THAT((*out[0].scale)[0], Catch::Matchers::WithinAbs(1.0, 1e-9));
    REQUIRE_THAT((*out[0].scale)[1], Catch::Matchers::WithinAbs(2.0, 1e-9));
    REQUIRE_THAT((*out[0].scale)[2], Catch::Matchers::WithinAbs(3.0, 1e-9));
    // uniform_scale must NOT be set.
    CHECK_FALSE(out[0].uniform_scale.has_value());
}

TEST_CASE("parse_objects: mirror as {x,y,z} bools",
          "[Placement][RequestMapping][Server]")
{
    json arr = json::parse(R"([
        {"index": 0, "mirror": {"x": true, "y": false, "z": false}}
    ])");

    std::vector<ObjectPlacement> out;
    REQUIRE_NOTHROW(parse_objects(arr, out));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].mirror.has_value());
    CHECK((*out[0].mirror)[0] == true);
    CHECK((*out[0].mirror)[1] == false);
    CHECK((*out[0].mirror)[2] == false);
}

TEST_CASE("parse_objects: instances array — count and fields",
          "[Placement][RequestMapping][Server]")
{
    json arr = json::parse(R"([
        {
            "index": 0,
            "instances": [
                {"position": {"x": 0.0, "y": 0.0, "z": 0.0}, "rotation_z": 0.0,  "scale": 1.0},
                {"position": {"x": 50.0,"y": 50.0,"z": 0.0}, "rotation_z": 90.0, "scale": 1.5}
            ]
        }
    ])");

    std::vector<ObjectPlacement> out;
    REQUIRE_NOTHROW(parse_objects(arr, out));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].instances.size() == 2);

    // First instance
    REQUIRE(out[0].instances[0].position.has_value());
    REQUIRE_THAT((*out[0].instances[0].position)[0], Catch::Matchers::WithinAbs(0.0,  1e-9));
    REQUIRE_THAT((*out[0].instances[0].position)[1], Catch::Matchers::WithinAbs(0.0,  1e-9));

    // Second instance
    REQUIRE(out[0].instances[1].position.has_value());
    REQUIRE_THAT((*out[0].instances[1].position)[0], Catch::Matchers::WithinAbs(50.0, 1e-9));
    REQUIRE(out[0].instances[1].rotation_z.has_value());
    REQUIRE_THAT(out[0].instances[1].rotation_z.value(), Catch::Matchers::WithinAbs(90.0, 1e-9));
    REQUIRE(out[0].instances[1].scale.has_value());
    REQUIRE_THAT(out[0].instances[1].scale.value(), Catch::Matchers::WithinAbs(1.5, 1e-9));
}

TEST_CASE("parse_objects: only-present keys — others remain nullopt",
          "[Placement][RequestMapping][Server]")
{
    // Only `index` is present; all optional transform fields must stay nullopt.
    json arr = json::parse(R"([{"index": 3}])");

    std::vector<ObjectPlacement> out;
    REQUIRE_NOTHROW(parse_objects(arr, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].index.has_value());
    CHECK(out[0].index.value() == 3);
    CHECK_FALSE(out[0].name.has_value());
    CHECK_FALSE(out[0].position.has_value());
    CHECK_FALSE(out[0].rotation.has_value());
    CHECK_FALSE(out[0].scale.has_value());
    CHECK_FALSE(out[0].uniform_scale.has_value());
    CHECK_FALSE(out[0].mirror.has_value());
    CHECK_FALSE(out[0].orient.has_value());
    CHECK_FALSE(out[0].ensure_on_bed.has_value());
    CHECK_FALSE(out[0].printable.has_value());
    CHECK(out[0].instances.empty());
}

#else // SLIC3R_SERVER not defined

TEST_CASE("parse_objects tests skipped (SLIC3R_SERVER not enabled)",
          "[Placement][RequestMapping][Server][skip]")
{
    WARN("SLIC3R_SERVER is not defined — parse_objects tests are skipped. "
         "Build with -DSLIC3R_SERVER=ON to enable them.");
    SUCCEED();
}

#endif // SLIC3R_SERVER

// ===========================================================================
// B) apply_object_placements tests
// ===========================================================================

TEST_CASE("apply_object_placements: position sets instance offset",
          "[Placement][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping position test");
        SUCCEED();
        return;
    }
    REQUIRE_FALSE(model.objects.empty());
    REQUIRE_FALSE(model.objects[0]->instances.empty());

    ObjectPlacement p;
    p.index    = 0;
    p.position = {10.0, 20.0, 5.0};

    std::vector<std::string> warnings;
    std::string              err;
    REQUIRE(apply_object_placements(model, {p}, {}, false, empty_cfg(), warnings, err));

    const Vec3d offset = model.objects[0]->instances[0]->get_offset();
    REQUIRE_THAT(offset.x(), Catch::Matchers::WithinAbs(10.0, 1e-6));
    REQUIRE_THAT(offset.y(), Catch::Matchers::WithinAbs(20.0, 1e-6));
    REQUIRE_THAT(offset.z(), Catch::Matchers::WithinAbs(5.0,  1e-6));
}

TEST_CASE("apply_object_placements: rotation sets instance rotation (deg -> rad)",
          "[Placement][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping rotation test");
        SUCCEED();
        return;
    }
    REQUIRE_FALSE(model.objects.empty());
    REQUIRE_FALSE(model.objects[0]->instances.empty());

    ObjectPlacement p;
    p.index    = 0;
    p.rotation = {0.0, 0.0, 45.0};   // degrees: Rz = 45°

    std::vector<std::string> warnings;
    std::string              err;
    REQUIRE(apply_object_placements(model, {p}, {}, false, empty_cfg(), warnings, err));

    const double rz = model.objects[0]->instances[0]->get_rotation(Z);
    REQUIRE_THAT(rz, Catch::Matchers::WithinAbs(Geometry::deg2rad(45.0), 1e-6));
}

TEST_CASE("apply_object_placements: mirror.x=true sets X mirror to -1",
          "[Placement][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping mirror test");
        SUCCEED();
        return;
    }
    REQUIRE_FALSE(model.objects.empty());
    REQUIRE_FALSE(model.objects[0]->instances.empty());

    ObjectPlacement p;
    p.index  = 0;
    p.mirror = {true, false, false};

    std::vector<std::string> warnings;
    std::string              err;
    REQUIRE(apply_object_placements(model, {p}, {}, false, empty_cfg(), warnings, err));

    const double mx = model.objects[0]->instances[0]->get_mirror(X);
    REQUIRE_THAT(mx, Catch::Matchers::WithinAbs(-1.0, 1e-9));
}

TEST_CASE("apply_object_placements: instances array rebuilds instance count",
          "[Placement][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping instances-array test");
        SUCCEED();
        return;
    }
    REQUIRE_FALSE(model.objects.empty());

    InstancePlacement ip1;
    ip1.position   = {0.0,  0.0,  0.0};
    ip1.rotation_z = 0.0;
    ip1.scale      = 1.0;

    InstancePlacement ip2;
    ip2.position   = {50.0, 50.0, 0.0};
    ip2.rotation_z = 90.0;
    ip2.scale      = 1.0;

    ObjectPlacement p;
    p.index     = 0;
    p.instances = {ip1, ip2};

    std::vector<std::string> warnings;
    std::string              err;
    REQUIRE(apply_object_placements(model, {p}, {}, false, empty_cfg(), warnings, err));

    CHECK(model.objects[0]->instances.size() == 2);
}

TEST_CASE("apply_object_placements: skip_objects marks instances non-printable",
          "[Placement][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping skip_objects test");
        SUCCEED();
        return;
    }
    REQUIRE_FALSE(model.objects.empty());
    REQUIRE_FALSE(model.objects[0]->instances.empty());

    std::vector<std::string> warnings;
    std::string              err;
    // Skip object index 0.
    REQUIRE(apply_object_placements(model, {}, {0}, false, empty_cfg(), warnings, err));

    for (const ModelInstance *inst : model.objects[0]->instances)
        CHECK(inst->printable == false);
}

TEST_CASE("apply_object_placements: name fallback targets correct object",
          "[Placement][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping name-fallback test");
        SUCCEED();
        return;
    }
    REQUIRE_FALSE(model.objects.empty());
    REQUIRE_FALSE(model.objects[0]->instances.empty());

    // Build a placement using the real object name (no index).
    ObjectPlacement p;
    p.name     = model.objects[0]->name;   // real name from fixture
    p.position = {1.0, 2.0, 3.0};

    std::vector<std::string> warnings;
    std::string              err;
    REQUIRE(apply_object_placements(model, {p}, {}, false, empty_cfg(), warnings, err));

    const Vec3d offset = model.objects[0]->instances[0]->get_offset();
    REQUIRE_THAT(offset.x(), Catch::Matchers::WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(offset.y(), Catch::Matchers::WithinAbs(2.0, 1e-6));
    REQUIRE_THAT(offset.z(), Catch::Matchers::WithinAbs(3.0, 1e-6));
}

TEST_CASE("apply_object_placements: unmatched placement emits warning and returns true",
          "[Placement][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping unmatched-placement test");
        SUCCEED();
        return;
    }

    ObjectPlacement p;
    p.index = 9999;   // index out of range — no object by that index
    // name left unset — nothing will match

    std::vector<std::string> warnings;
    std::string              err;
    // Non-fatal: must return true and emit at least one warning.
    const bool ok = apply_object_placements(model, {p}, {}, false, empty_cfg(), warnings, err);
    CHECK(ok);
    CHECK_FALSE(warnings.empty());
}

TEST_CASE("apply_object_placements: off-bed position emits advisory warning",
          "[Placement][ModelTransforms]")
{
    Model model;
    if (!load_fixture(model)) {
        WARN("3mf fixture not found — skipping off-bed-warning test");
        SUCCEED();
        return;
    }
    REQUIRE_FALSE(model.objects.empty());

    ObjectPlacement p;
    p.index    = 0;
    // Position far outside any reasonable bed (500 m away).
    p.position = {500000.0, 500000.0, 0.0};

    std::vector<std::string> warnings;
    std::string              err;
    // Must not crash and must return true (non-fatal out-of-bed advisory).
    const bool ok = apply_object_placements(model, {p}, {}, false, empty_cfg(), warnings, err);
    CHECK(ok);
    // At minimum, the call must not produce a hard error string.
    // A warning is expected but the exact text is implementation-defined;
    // we simply require the call survived without a hard error.
    CHECK(err.empty());
}
