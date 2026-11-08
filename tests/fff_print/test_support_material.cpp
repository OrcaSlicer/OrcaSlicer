#include <algorithm>
#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include <cmath>
#include <limits>
#include <cstddef>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include <map>
#include <math.h>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_helpers.hpp" // get access to init_print, etc

// Not self-contained: its inline constructor uses PrintObject, PrintRegion, SlicingParameters and
// Geometry, so it must follow the headers (pulled in via test_helpers.hpp) that define them.
#include "libslic3r/Support/SupportParameters.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Print.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

// Distinct layer Z heights carrying support interface extrusion.
static size_t support_interface_layer_count(const std::string &gcode)
{
    return layers_with_role(gcode, "support material interface").size();
}

// Distinct layer Z heights carrying support base extrusion. The base G-code label "support material"
// is a substring of "support material interface", so a base line is a support line that is not an
// interface line.
static size_t support_base_layer_count(const std::string &gcode)
{
    std::set<double> layers;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&layers](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        const std::string_view comment = line.comment();
        if (comment.find("support material") != std::string_view::npos &&
            comment.find("interface") == std::string_view::npos)
            layers.insert(self.z());
    });
    return layers.size();
}

// Dominant support-interface fill direction per interface layer, in radians [0, pi). Uses the
// length-weighted axial mean (each segment angle doubled so a line and its reverse agree, then
// halved): the parallel infill lines reinforce while the surrounding perimeter cancels.
static std::map<double, double> interface_fill_angle_by_layer(const std::string &gcode)
{
    std::map<double, std::pair<double, double>> acc; // z -> summed length*(cos2a, sin2a)
    GCodeReader parser;
    parser.parse_buffer(gcode, [&acc](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        if (line.comment().find("support material interface") == std::string_view::npos) return;
        const double dx = line.dist_X(self), dy = line.dist_Y(self);
        const double len = std::hypot(dx, dy);
        if (len < 1e-6) return;
        const double a2 = 2.0 * std::atan2(dy, dx);
        auto &p = acc[self.z()];
        p.first  += len * std::cos(a2);
        p.second += len * std::sin(a2);
    });
    std::map<double, double> out;
    for (const auto &kv : acc) {
        double a = 0.5 * std::atan2(kv.second.second, kv.second.first);
        if (a < 0) a += M_PI;
        out[kv.first] = a;
    }
    return out;
}

// Acute angle (degrees) between two axial fill directions in [0, pi).
static double axial_angle_diff_deg(double a, double b)
{
    const double d = std::fmod(std::fabs(a - b), M_PI);
    return std::min(d, M_PI - d) * 180.0 / M_PI;
}

// Denser interface spacing yields more extruded length.
static double support_interface_extrusion_length(const std::string &gcode)
{
    double len = 0;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&len](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        if (line.comment().find("support material interface") == std::string_view::npos) return;
        len += std::hypot(line.dist_X(self), line.dist_Y(self));
    });
    return len;
}

// A cap slab overhanging a base, joined by a central stem: the cap can only be supported by resting on the
// base, forcing a genuine bottom contact. A horizontal tunnel does not work here -- tree/organic can arch a
// branch in from the opening and avoid the floor entirely.
static TriangleMesh support_capital()
{
    TriangleMesh model = make_cube(40, 40, 2);                              // base  [0,40]x[0,40]x[0,2]
    TriangleMesh stem  = make_cube(8, 8, 12);   stem.translate(16, 16, 1);  // stem  centered, z 1..13
    TriangleMesh cap   = make_cube(40, 40, 2);  cap.translate(0, 0, 12);    // cap   z 12..14
    model.merge(stem);
    model.merge(cap);
    return model;
}

TEST_CASE("Three raft layers are created", "[SupportMaterial]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ cube(20) }, print, {
        { "enable_support", 1 },
        { "raft_layers",    3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

TEST_CASE("Enforced support layers are generated", "[SupportMaterial]")
{
    // enforce_support_layers forces support on the first N layers even with support off.
    Slic3r::Print baseline;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, baseline, {
        { "enable_support",         0 },
        { "enforce_support_layers", 0 }
    });
    REQUIRE(baseline.objects().front()->support_layers().empty());

    Slic3r::Print enforced;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, enforced, {
        { "enable_support",         0 },
        { "enforce_support_layers", 100 }
    });
    REQUIRE(enforced.objects().front()->support_layers().size() > 0);
}

// Support-needed statuses raised while slicing support_capital() with support off. The CLI lists these
// in result.json and fails on them under --strict. Collected under a lock: generate_support_material()
// runs on TBB workers.
static std::vector<PrintBase::SlicingStatus> support_needed_statuses(bool no_check)
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({ support_capital() }, print, model, {
        { "enable_support",         0 },
        { "enforce_support_layers", 0 }
    });
    print.set_no_check_flag(no_check);

    std::mutex mutex;
    std::vector<PrintBase::SlicingStatus> statuses;
    print.set_status_callback([&mutex, &statuses](const PrintBase::SlicingStatus &status) {
        if (status.message_type != PrintStateBase::SlicingNeedSupportOn)
            return;
        std::lock_guard<std::mutex> lock(mutex);
        statuses.push_back(status);
    });
    print.process();
    return statuses;
}

TEST_CASE("An overhang sliced with support off reports that support is needed", "[SupportMaterial]")
{
    // The 40mm cap reaches ~22mm past its 8mm stem, beyond the 6mm cantilever limit of
    // PrintObject::is_support_necessary().
    const std::vector<PrintBase::SlicingStatus> statuses = support_needed_statuses(false);
    REQUIRE(! statuses.empty());
    for (const PrintBase::SlicingStatus &status : statuses) {
        // The CLI only considers step warnings (warning_step != -1), and --strict only NON_CRITICAL ones.
        CHECK(status.warning_level == PrintStateBase::WarningLevel::NON_CRITICAL);
        CHECK(status.warning_step != -1);
    }
}

TEST_CASE("The no-check flag skips the support-needed check", "[SupportMaterial]")
{
    CHECK(support_needed_statuses(true).empty());
}

SCENARIO("Support layer Z honors contact distance", "[SupportMaterial]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok)
	{
        ConstSupportLayerPtrsAdaptor support_layers = print.objects().front()->support_layers();

		first_support_layer_height_ok = support_layers.front()->print_z == print.config().initial_layer_print_height.value;

		layer_height_minimum_ok = true;
		layer_height_maximum_ok = true;
		double min_layer_height = print.config().min_layer_height.values.front();
		double max_layer_height = print.config().nozzle_diameter.values.front();
		if (print.config().max_layer_height.values.front() > EPSILON)
			max_layer_height = std::min(max_layer_height, print.config().max_layer_height.values.front());
		for (size_t i = 1; i < support_layers.size(); ++ i) {
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z < min_layer_height - EPSILON)
				layer_height_minimum_ok = false;
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z > max_layer_height + EPSILON)
				layer_height_maximum_ok = false;
		}
	};

    GIVEN("A print object having one modelObject") {
        WHEN("Layer height = 0.2 and first layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.4 },
                { "dont_support_bridges",       false },
			});
			bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
        WHEN("Layer height = 0.2 and first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.3 },
                { "dont_support_bridges",       false },
            });
            bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
    }
}

// extrude_support once held a `static` lambda capturing `this`, so a second export in the
// same process dereferenced a returned stack frame (ASan: stack-use-after-return).
TEST_CASE("Support G-code emission survives a second slice in the same process", "[SupportMaterial][Regression]")
{
    const std::string first = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(first, "support").empty());

    const std::string second = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(second, "support").empty());
}

// The contact layer counts toward the configured interface layer count, so N configured top
// interface layers produce exactly N interface layers, not N+1.
TEST_CASE("Support top interface layer count matches the configured value", "[SupportMaterial]")
{
    const int top = GENERATE(1, 2, 3, 4, 6);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     1 },
        { "support_interface_top_layers",    top },
        { "support_interface_bottom_layers", 0 },
    });
    CAPTURE(top);
    REQUIRE(support_base_layer_count(g)      > 0);          // support actually formed
    REQUIRE(support_interface_layer_count(g) == size_t(top));
}

// A rotated cube-with-hole is a horizontal tunnel whose ceiling and floor both receive support, so top
// and bottom interfaces can be exercised independently (the floor is the bottom contact).
static TriangleMesh support_tunnel()
{
    TriangleMesh tunnel = Slic3r::Test::mesh(TestMesh::cube_with_hole);
    tunnel.rotate_x(float(M_PI / 2));
    return tunnel;
}

static size_t tunnel_interface_layers(const TriangleMesh &tunnel, int top, int bottom)
{
    const std::string g = slice({ tunnel }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     0 },
        { "support_interface_top_layers",    top },
        { "support_interface_bottom_layers", bottom },
    });
    REQUIRE(support_base_layer_count(g) > 0); // support actually formed
    return support_interface_layer_count(g);
}

TEST_CASE("No support interface is generated when neither top nor bottom is configured", "[SupportMaterial]")
{
    REQUIRE(tunnel_interface_layers(support_tunnel(), 0, 0) == 0);
}

TEST_CASE("Bottom interface layer count matches its setting with top interface off", "[SupportMaterial]")
{
    const int bottom = GENERATE(1, 3, 6);
    CAPTURE(bottom);
    REQUIRE(tunnel_interface_layers(support_tunnel(), 0, bottom) == size_t(bottom));
}

// support_interface_bottom_layers = -1 means "same as top".
TEST_CASE("Support interface bottom layers default to the top layer count", "[SupportMaterial]")
{
    const TriangleMesh tunnel = support_tunnel();
    REQUIRE(tunnel_interface_layers(tunnel, 0, -1) == tunnel_interface_layers(tunnel, 0, 0));
    REQUIRE(tunnel_interface_layers(tunnel, 3, -1) == tunnel_interface_layers(tunnel, 3, 3));
}

TEST_CASE("Default support still emits base and interface material", "[SupportMaterial][Regression]")
{
    const std::string g = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(support_base_layer_count(g)      > 0);
    REQUIRE(support_interface_layer_count(g) > 0);
}

// Organic runs TreeSupport3D + TreeModelVolumes, the others the classic TreeSupport.cpp path.
TEST_CASE("Every tree support style produces base and interface material", "[SupportMaterial]")
{
    const char *style = GENERATE("organic", "tree_slim", "tree_strong", "tree_hybrid");
    INFO("style=" << style);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",               1 },
        { "layer_height",                 0.2 },
        { "support_type",                 "tree(auto)" },
        { "support_style",                style },
        { "support_interface_top_layers", 3 },
    });
    CHECK(support_base_layer_count(g)      > 0);
    CHECK(support_interface_layer_count(g) > 0);
}

TEST_CASE("Raft interface angle alternates by 45 degrees per interface id", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, { { "enable_support", 1 } });
    SupportParameters sp(*print.objects().front());
    sp.raft_angle_interface = 0.5f;
    REQUIRE_THAT(sp.raft_interface_angle(0), Catch::Matchers::WithinAbs(0.5 + M_PI / 4., 1e-6));
    REQUIRE_THAT(sp.raft_interface_angle(1), Catch::Matchers::WithinAbs(0.5 - M_PI / 4., 1e-6));
}

// The angle inputs are overwritten directly, so the pattern-to-angle mapping is checked
// independently of the sliced object's configuration.
TEST_CASE("Support interface fill angle follows the configured interface pattern", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, { { "enable_support", 1 } });
    SupportParameters sp(*print.objects().front());
    sp.interface_angle = 0.3f;
    sp.base_angle      = 1.1f;
    const double tol   = 1e-6;

    SECTION("Rectilinear shifts the interface angle by -45deg for snug support") {
        sp.support_interface_pattern = smipRectilinear;
        sp.support_style             = smsSnug;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
        REQUIRE_THAT(sp.support_interface_angle(3), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
    }
    SECTION("Rectilinear leaves the interface angle alone for the other styles") {
        sp.support_interface_pattern = smipRectilinear;
        sp.support_style             = smsGrid;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
    }
    SECTION("Rectilinear interlaced alternates -/+45deg by interface id parity") {
        sp.support_interface_pattern = smipRectilinearInterlaced;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
        REQUIRE_THAT(sp.support_interface_angle(1), Catch::Matchers::WithinAbs(sp.interface_angle + M_PI_4, tol));
    }
    SECTION("Grid uses the base angle") {
        sp.support_interface_pattern = smipGrid;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.base_angle, tol));
    }
    SECTION("Auto and concentric use the interface angle unchanged") {
        sp.support_interface_pattern = smipAuto;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
        sp.support_interface_pattern = smipConcentric;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
    }
}

// End-to-end that the pattern reaches the emitted fill, not just support_interface_angle().
TEST_CASE("Interlaced support interface alternates fill angle while rectilinear does not", "[SupportMaterial]")
{
    auto interface_angles = [](const char *pattern) {
        std::vector<double> a;
        for (const auto &kv : interface_fill_angle_by_layer(slice({ TestMesh::overhang }, {
                 { "enable_support",               1 },
                 { "layer_height",                 0.2 },
                 { "support_on_build_plate_only",  1 },
                 { "support_interface_top_layers", 6 },
                 { "support_interface_pattern",    pattern } })))
            a.push_back(kv.second);
        return a;
    };

    const std::vector<double> rectilinear = interface_angles("rectilinear");
    const std::vector<double> interlaced  = interface_angles("rectilinear_interlaced");
    REQUIRE(rectilinear.size() >= 3);
    REQUIRE(interlaced.size()  >= 3);

    for (size_t i = 1; i < rectilinear.size(); ++i)
        REQUIRE(axial_angle_diff_deg(rectilinear[i], rectilinear[0]) < 15.0);

    for (size_t i = 1; i < interlaced.size(); ++i)
        REQUIRE(axial_angle_diff_deg(interlaced[i], interlaced[i - 1]) > 60.0);
}

// Normal and non-organic tree support share the same interface angle logic: with a rectilinear interface
// pattern both emit their interface fill at the same angle (both go through support_interface_angle()).
TEST_CASE("Normal and tree support use the same interface fill angle", "[SupportMaterial]")
{
    auto mean_interface_angle = [](const char *type, const char *style) {
        const auto angles = interface_fill_angle_by_layer(slice({ TestMesh::overhang }, {
            { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 1 },
            { "support_type", type }, { "support_style", style },
            { "support_interface_top_layers", 6 }, { "support_interface_pattern", "rectilinear" } }));
        REQUIRE(angles.size() >= 3);
        // Axial mean, as in interface_fill_angle_by_layer: a plain mean would split angles either
        // side of the [0, pi) wrap.
        double x = 0, y = 0;
        for (const auto &kv : angles) {
            x += std::cos(2.0 * kv.second);
            y += std::sin(2.0 * kv.second);
        }
        double mean = 0.5 * std::atan2(y, x);
        if (mean < 0) mean += M_PI;
        return mean;
    };
    REQUIRE(axial_angle_diff_deg(mean_interface_angle("normal(auto)", "default"),
                                 mean_interface_angle("tree(auto)", "tree_slim")) < 10.0);
}

// Every style, because the non-organic tree styles once emitted one more top interface layer than the rest.
TEST_CASE("Top interface layer count equals the configured value for every support style", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "grid" },        { "normal(auto)", "snug" },
        { "tree(auto)",   "organic" },     { "tree(auto)",   "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)",   "tree_hybrid" },
    }));
    CAPTURE(style);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",               1 },
        { "layer_height",                 0.2 },
        { "support_type",                 type },
        { "support_style",                style },
        { "support_interface_top_layers", 4 },
    });
    REQUIRE(support_interface_layer_count(g) == 4u);
}

// The bottom interface was dropped in earlier versions when support started on the model rather
// than the plate.
TEST_CASE("Non-organic tree support generates a bottom interface on internal geometry", "[SupportMaterial]")
{
    const std::string g = slice({ support_tunnel() }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     0 },
        { "support_type",                    "tree(auto)" },
        { "support_style",                   "tree_slim" },
        { "support_interface_top_layers",    0 },
        { "support_interface_bottom_layers", 6 },
    });
    REQUIRE(support_base_layer_count(g)      > 0);
    REQUIRE(support_interface_layer_count(g) > 0);
}

// The capital forces the model contact; on a horizontal tunnel organic can arch a branch in and make none.
TEST_CASE("A bottom interface is produced for every support style on a forced model contact", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "default" },     { "tree(auto)", "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)", "tree_hybrid" },
        { "tree(auto)",   "organic" },
    }));
    CAPTURE(style);
    REQUIRE(support_interface_layer_count(slice({ support_capital() }, {
        { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 0 },
        { "support_type", type }, { "support_style", style },
        { "support_interface_top_layers", 0 }, { "support_interface_bottom_layers", 6 } })) > 0);
}

TEST_CASE("Bottom interface spacing controls bottom interface density for every support style", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "default" },     { "tree(auto)", "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)", "tree_hybrid" },
        { "tree(auto)",   "organic" },
    }));
    CAPTURE(style);
    const TriangleMesh model = support_capital();
    auto len = [&model](const char *support_type, const char *support_style, double spacing) {
        return support_interface_extrusion_length(slice({ model }, {
            { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 0 },
            { "support_type", support_type }, { "support_style", support_style }, { "support_interface_top_layers", 0 },
            { "support_interface_bottom_layers", 6 }, { "support_bottom_interface_spacing", spacing } }));
    };
    REQUIRE(len(type, style, 0.0) > len(type, style, 4.0) * 1.5);
}

// Interface and base flows are identical in width and rate unless a separate support-interface
// filament is used, so density is the observable here, not flow.
TEST_CASE("Bottom-only support interface keeps the dense interface density", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, {
        { "enable_support",                   1 },
        { "support_interface_top_layers",     0 },
        { "support_interface_bottom_layers",  6 },
        { "support_bottom_interface_spacing", 0.0 },  // solid: density resolves to 1.0
        { "support_base_pattern_spacing",     2.5 },  // sparse: density stays below 1.0
    });
    SupportParameters sp(*print.objects().front());
    REQUIRE(sp.bottom_interface_density > sp.support_density);
}


TEST_CASE("A support enforcer volume follows a rotated object's slice frame", "[SupportMaterial]")
{
    // Enforcer/blocker volumes are cut in the same frame as the model parts. With the layers
    // tilted (non-planar layer_rotation_b) that frame is rotated, and an enforcer cut in the
    // unrotated frame landed several millimetres away from the face it was painted under.
    const double tilt = 20.;
    Slic3r::Print print;
    Model         model;
    ModelObject  *object = model.add_object();
    object->name = "cantilever_with_enforcer";
    TriangleMesh pillar = make_cube(8., 8., 20.);
    TriangleMesh beam   = make_cube(40., 8., 2.);
    beam.translate(0.f, 0.f, 18.f);
    pillar.merge(beam);
    object->add_volume(std::move(pillar), ModelVolumeType::MODEL_PART, false);
    // A small enforcer box straddling the beam's underside at its free end: an enforcer marks the
    // overhang it contains, so it has to contain the underside layer itself.
    ModelVolume *enforcer = object->add_volume(make_cube(6., 6., 4.), ModelVolumeType::SUPPORT_ENFORCER, false);
    enforcer->set_offset(Vec3d(32., 1., 15.));
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar",  "1" },
        { "layer_rotation_b",  - tilt },
        { "enable_support",    "1" },
        { "support_type",      "normal(manual)" },   // enforcers only
        { "layer_height",      "0.2" },
        { "skirt_loops",       "0" },
        { "layer_change_gcode", "G92 E0" },
    });
    print.apply(model, config);
    print.process();
    REQUIRE(print.objects().size() == 1);
    const PrintObject &po = *print.objects().front();
    REQUIRE(! po.support_layers().empty());

    // Where the enforcer's centre lands in the slice frame; the support it forces must be there too.
    const Vec3d centre_slice = po.object_to_slice() * (enforcer->get_matrix() * Vec3d(3., 3., 2.));
    BoundingBox support_extents;
    for (const SupportLayer *layer : po.support_layers())
        for (const ExPolygon &island : layer->support_islands)
            support_extents.merge(get_extents(island));
    REQUIRE(support_extents.defined);
    const Vec2d support_centre = unscale(support_extents.center());
    INFO("enforcer centre in slice frame " << centre_slice.transpose() << ", support centre " << support_centre.transpose());
    CHECK_THAT(support_centre.x(), Catch::Matchers::WithinAbs(centre_slice.x(), 4.));
    CHECK_THAT(support_centre.y(), Catch::Matchers::WithinAbs(centre_slice.y(), 4.));
}

TEST_CASE("Support of a whole-object layer rotation prints under the part and above the bed", "[SupportMaterial][Regression]")
{
    // An ordinary object rotated by layer_rotation_b/c is sliced in a rotated frame and its islands
    // are un-rotated on the way out. Its support was emitted in the slice frame as it stood, so it
    // came out turned 90 degrees away from the part it was holding up.
    const auto [b, c] = GENERATE(table<double, double>({ { 90., 0. }, { 90., 90. } }));
    const std::string mode = GENERATE(as<std::string>{}, "bc", "virtual");
    INFO("layer_rotation_b " << b << ", layer_rotation_c " << c << ", printer type " << mode);

    // A plate with a tower on each corner: whichever horizontal direction the rotation turns the
    // build direction to, the towers on the far side start as an overhang off the plate. The plate
    // is twice as long as the towers are tall, so support turned 90 degrees cannot fit the part's
    // extent by accident.
    TriangleMesh mesh = make_cube(40., 20., 4.);
    for (const Vec2f corner : { Vec2f(0.f, 0.f), Vec2f(34.f, 0.f), Vec2f(0.f, 14.f), Vec2f(34.f, 14.f) }) {
        TriangleMesh tower = make_cube(6., 6., 20.);
        tower.translate(corner.x(), corner.y(), 0.f);
        mesh.merge(tower);
    }
    Slic3r::Print print;
    Model         model;
    ModelObject  *object = model.add_object();
    object->add_volume(std::move(mesh));
    object->add_instance()->set_offset(Vec3d(80., 80., 0.));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar",   "1" },
        { "axis_selection",     mode },
        // Virtual mode: no hotend envelope, so the 90-degree tilt is not refused by the tilt limit.
        { "nonplanar_hotend_cone_height", 0 },
        { "layer_rotation_b",   b },
        { "layer_rotation_c",   c },
        { "enable_support",     "1" },
        { "support_type",       "normal(auto)" },
        { "layer_height",       "0.2" },
        { "skirt_loops",        "0" },
        { "brim_type",          "no_brim" },
        { "layer_change_gcode", "G92 E0" },
    });
    print.apply(model, config);
    print.process();
    REQUIRE(! print.objects().front()->support_layers().empty());
    const std::string gcode = Slic3r::Test::gcode(print);

    BoundingBoxf3 part;
    std::vector<Vec3d> support_pts;
    bool in_support = false, in_part = false;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.rfind("TYPE:", 0) == 0) {
            in_support = comment.find("Support") != std::string_view::npos;
            in_part    = ! in_support && comment.find("wall") != std::string_view::npos;
        }
        if (! line.extruding(self) || line.dist_XY(self) <= 0.)
            return;
        const Vec3d p(line.new_X(self), line.new_Y(self), line.new_Z(self));
        if (in_support)
            support_pts.emplace_back(p);
        else if (in_part)
            part.merge(p);
    });
    REQUIRE(part.defined);
    REQUIRE(! support_pts.empty());

    // Support reaches past the overhang it holds up by its expansion margin, so allow a little more
    // than that around the part; support turned 90 degrees away lands well outside it.
    const BoundingBoxf3 near_part(part.min - Vec3d(2., 2., 2.), part.max + Vec3d(2., 2., 2.));
    size_t inside = 0;
    double lowest = std::numeric_limits<double>::max();
    for (const Vec3d &p : support_pts) {
        if (near_part.contains(p))
            ++ inside;
        lowest = std::min(lowest, p.z());
    }
    INFO("part walls " << part.min.transpose() << " .. " << part.max.transpose() << ", "
         << inside << " of " << support_pts.size() << " support points near it, lowest support z " << lowest);
    CHECK(inside == support_pts.size());
    CHECK(lowest >= 0.);
}
