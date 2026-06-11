// test_json_request.cpp
//
// Unit tests for json_to_slice_request() — the JSON body -> SliceRequest
// mapping function extracted into RequestMapping.hpp / .cpp.
//
// Compiled only when SLIC3R_SERVER is defined (mirrors the guard used by
// test_server_e2e.cpp).  A placeholder TEST_CASE is provided for the
// SLIC3R_SERVER=OFF build so the test binary still registers at least one test.
//
// Catch2 version: v3.11.0

#include <catch2/catch_all.hpp>

#ifdef SLIC3R_SERVER

#include "RequestMapping.hpp"     // Slic3r::Server::json_to_slice_request

#include "libslic3r/Config.hpp"   // ConfigOptionFloat, ConfigOptionInt, ConfigOptionBool

#include <nlohmann/json.hpp>

using json = nlohmann::json;

using namespace Slic3r;
using namespace Slic3r::Server;
using namespace Slic3r::SliceCore;

// ---------------------------------------------------------------------------
// TEST: parse_overrides_basic
//
// Verifies that a "presets.overrides" JSON object containing a string value
// (layer_height), an integer value (wall_loops), and a boolean value
// (spiral_mode) are each deserialised into req.presets.overrides.
// ---------------------------------------------------------------------------
TEST_CASE("parse_overrides_basic", "[RequestMapping][Server]")
{
    json j = json::parse(R"({
        "presets": {
            "overrides": {
                "layer_height": "0.2",
                "wall_loops":   2,
                "spiral_mode":  false
            }
        }
    })");

    SliceRequest req = json_to_slice_request(j);

    // layer_height is a float key — must be present and close to 0.2.
    REQUIRE(req.presets.overrides.has("layer_height"));
    const auto *lh = req.presets.overrides.opt<ConfigOptionFloat>("layer_height");
    REQUIRE(lh != nullptr);
    REQUIRE_THAT(lh->value, Catch::Matchers::WithinAbs(0.2, 1e-6));

    // wall_loops is an int key.
    REQUIRE(req.presets.overrides.has("wall_loops"));
    const auto *wl = req.presets.overrides.opt<ConfigOptionInt>("wall_loops");
    REQUIRE(wl != nullptr);
    REQUIRE(wl->value == 2);

    // spiral_mode is a bool key.
    REQUIRE(req.presets.overrides.has("spiral_mode"));
    const auto *sm = req.presets.overrides.opt<ConfigOptionBool>("spiral_mode");
    REQUIRE(sm != nullptr);
    REQUIRE(sm->value == false);
}

// ---------------------------------------------------------------------------
// TEST: parse_overrides_unknown_key_tolerant
//
// An unrecognised override key must NOT throw.  The lenient
// ForwardCompatibilitySubstitutionRule::Enable path is exercised.
// ---------------------------------------------------------------------------
TEST_CASE("parse_overrides_unknown_key_tolerant", "[RequestMapping][Server]")
{
    json j = json::parse(R"({
        "presets": {
            "overrides": {
                "totally_bogus_key_that_does_not_exist": "some_value"
            }
        }
    })");

    REQUIRE_NOTHROW(json_to_slice_request(j));
}

// ---------------------------------------------------------------------------
// TEST: parse_transforms_flags
//
// Verifies GAP 4: rotate_x, rotate_y, ensure_on_bed, convert_unit, and
// assemble are parsed correctly from the "transforms" JSON object.
// ---------------------------------------------------------------------------
TEST_CASE("parse_transforms_flags", "[RequestMapping][Server]")
{
    json j = json::parse(R"({
        "transforms": {
            "rotate_x":      45.0,
            "rotate_y":      30.0,
            "ensure_on_bed": true,
            "convert_unit":  true,
            "assemble":      false
        }
    })");

    SliceRequest req = json_to_slice_request(j);

    REQUIRE_THAT(req.transforms.rotate_x,    Catch::Matchers::WithinAbs(45.0, 1e-9));
    REQUIRE_THAT(req.transforms.rotate_y,    Catch::Matchers::WithinAbs(30.0, 1e-9));
    REQUIRE(req.transforms.ensure_on_bed == true);
    REQUIRE(req.transforms.convert_unit  == true);
    REQUIRE(req.transforms.assemble      == false);
}

// ---------------------------------------------------------------------------
// TEST: parse_export_kind
//
// Regression guard for the pre-existing export.kind mapping:
//   "3mf"  -> ExportKind::ThreeMF
//   "stl"  -> ExportKind::Stl
//   absent -> ExportKind::Gcode   (default)
// ---------------------------------------------------------------------------
TEST_CASE("parse_export_kind", "[RequestMapping][Server]")
{
    SECTION("3mf maps to ThreeMF") {
        json j = json::parse(R"({"export": {"kind": "3mf"}})");
        SliceRequest req = json_to_slice_request(j);
        REQUIRE(req.export_kind == ExportKind::ThreeMF);
    }

    SECTION("stl maps to Stl") {
        json j = json::parse(R"({"export": {"kind": "stl"}})");
        SliceRequest req = json_to_slice_request(j);
        REQUIRE(req.export_kind == ExportKind::Stl);
    }

    SECTION("absent export key defaults to Gcode") {
        json j = json::parse(R"({})");
        SliceRequest req = json_to_slice_request(j);
        REQUIRE(req.export_kind == ExportKind::Gcode);
    }
}

#else // SLIC3R_SERVER not defined

// When SLIC3R_SERVER=OFF the server sources are not compiled and no server
// tests are registered.  Provide one placeholder test so that the test binary
// does not fail with "no tests ran".

TEST_CASE("request mapping tests skipped (SLIC3R_SERVER not enabled)",
          "[RequestMapping][Server][skip]")
{
    WARN("SLIC3R_SERVER is not defined — all RequestMapping tests are skipped. "
         "Build with -DSLIC3R_SERVER=ON to enable them.");
    SUCCEED();
}

#endif // SLIC3R_SERVER
