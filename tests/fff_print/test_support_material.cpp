#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include <algorithm>
#include <memory>
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "test_helpers.hpp" // get access to init_print, etc

// Not self-contained: its inline constructor uses PrintObject, PrintRegion, SlicingParameters and
// Geometry, so it must follow the headers (pulled in via test_helpers.hpp) that define them.
#include "libslic3r/Support/SupportParameters.hpp"
#include "libslic3r/Support/SupportFins.hpp"

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

// A 30 x 30 x 2 mm slab held 10 mm above the bed by a 10 x 10 mm stem: the slab hangs over the bed
// all around the stem, and fins crossing the stem end right at its walls.
static TriangleMesh support_table()
{
    TriangleMesh model = make_cube(30, 30, 2);  model.translate(0, 0, 10);   // slab z 10..12
    TriangleMesh stem  = make_cube(10, 10, 11); stem.translate(10, 10, 0);   // stem z 0..11
    model.merge(stem);
    return model;
}

// Y of the area centroid of `expolys`.
static double area_centroid_y(const ExPolygons &expolys, double &area)
{
    double sum = 0.;
    area = 0.;
    for (const ExPolygon &expoly : expolys) {
        sum  += expoly.area() * get_extents(expoly).center().y();
        area += expoly.area();
    }
    return area > 0. ? sum / area : 0.;
}

// A 20 mm cube tipped 45 degrees about X onto an edge: its lower faces are exactly 45 degrees.
static TriangleMesh cube_on_edge()
{
    TriangleMesh cube = make_cube(20, 20, 20);
    cube.translate(-10, -10, -10);
    cube.rotate_x(float(M_PI / 4));
    cube.translate(0, 0, float(- cube.bounding_box().min.z()));
    return cube;
}

TEST_CASE("Fin support is built from walls no thicker than the fin thickness", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ support_table() }, print, {
        { "enable_support",            1 },
        { "support_type",              "fins" },
        { "layer_height",              0.2 },
        { "support_fin_spacing",       6 },
        // Fins are whole two-line loops; with the 0.4 mm default line this is one loop, about 0.7 mm.
        { "support_fin_thickness",     0.7 },
        { "support_fin_tine_spacing", 0 },
    });
    size_t checked = 0;
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        // The fin foot within 1 mm of the bed is wider by design.
        if (layer->print_z < 1.5)
            continue;
        // Any region wider than one loop (at most 0.8 mm) survives an opening by half of that width.
        CAPTURE(layer->print_z);
        CHECK(opening(layer->support_islands, float(scale_(0.5 * 0.8 + 0.05))).empty());
        ++ checked;
    }
    REQUIRE(checked > 0);
}

TEST_CASE("Fin support touches the object only on tine layers", "[SupportMaterial][Fins]")
{
    const double tine_spacing = 3.;
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ support_table() }, print, {
        { "enable_support",             1 },
        { "support_type",               "fins" },
        { "layer_height",               0.2 },
        { "support_object_xy_distance", 0.3 },
        { "support_fin_spacing",        4 },
        { "support_fin_thickness",      1.2 },
        { "support_fin_tine_spacing",   tine_spacing },
        { "support_fin_tine_depth",     0.3 },
    });
    const PrintObject &object = *print.objects().front();
    const std::vector<bool> tine_row = fin_tine_layers(object);
    size_t tine_layers = 0;
    for (const SupportLayer *layer : object.support_layers()) {
        const Layer *object_layer = object.get_layer_at_printz(layer->print_z, EPSILON);
        if (object_layer == nullptr)
            continue;
        // Ignore numerical slivers; a tine is about 0.8 x 0.3 mm = 0.24 mm^2.
        const bool touches = area(intersection_ex(layer->support_islands, object_layer->lslices)) > scale_(scale_(0.05));
        CAPTURE(layer->print_z);
        if (tine_row[object_layer->id()])
            tine_layers += touches;
        else
            CHECK_FALSE(touches);
    }
    REQUIRE(tine_layers > 0);
}

TEST_CASE("Fin support without tines never touches the object", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ support_table() }, print, {
        { "enable_support",            1 },
        { "support_type",              "fins" },
        { "layer_height",              0.2 },
        { "support_fin_tine_spacing", 0 },
    });
    const PrintObject &object = *print.objects().front();
    REQUIRE(! object.support_layers().empty());
    for (const SupportLayer *layer : object.support_layers())
        if (const Layer *object_layer = object.get_layer_at_printz(layer->print_z, EPSILON)) {
            CAPTURE(layer->print_z);
            CHECK(area(intersection_ex(layer->support_islands, object_layer->lslices)) < scale_(scale_(0.05)));
        }
}

// Normal support at a 30 degree threshold finds nothing on 45 degree faces; fins do not use the threshold.
TEST_CASE("Fin support holds a cube tipped onto an edge regardless of the overhang threshold", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ cube_on_edge() }, print, {
        { "enable_support",          1 },
        { "support_type",            "fins" },
        { "layer_height",            0.2 },
        { "support_threshold_angle", 30 },
    });
    const PrintObject &object = *print.objects().front();
    REQUIRE(! object.support_layers().empty());
    // The cube is widest at 14.1 mm (20 mm * sqrt(2) / 2); fins reach most of the way up.
    REQUIRE(object.support_layers().back()->print_z > 10.);

    // The fins cross the edge the cube rests on (along X), so each fin runs along Y.
    const SupportLayer *mid = nullptr;
    for (const SupportLayer *layer : object.support_layers())
        if (layer->print_z > 5.)
            { mid = layer; break; }
    REQUIRE(mid != nullptr);
    REQUIRE(! mid->support_islands.empty());
    for (const ExPolygon &fin : mid->support_islands) {
        const BoundingBox b = get_extents(fin);
        CHECK(b.size().y() > b.size().x());
    }
}

TEST_CASE("Fin support stops at the fin height", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ cube_on_edge() }, print, {
        { "enable_support",     1 },
        { "support_type",       "fins" },
        { "layer_height",       0.2 },
        { "support_fin_height", 4 },
    });
    const PrintObject &object = *print.objects().front();
    REQUIRE(! object.support_layers().empty());
    REQUIRE(object.support_layers().back()->print_z <= 4. + EPSILON);
}

TEST_CASE("Fin support on a raft is rejected", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({ cube_on_edge() }, print, model, {
        { "enable_support", 1 },
        { "support_type",   "fins" },
        { "raft_layers",    2 },
    });
    const StringObjectException error = print.validate();
    CHECK(error.opt_key == "raft_layers");
    CHECK(error.string.find("Fin support") != std::string::npos);
}

TEST_CASE("Fin support on the lean side only keeps one side of a balanced part", "[SupportMaterial][Fins]")
{
    // The cube on its edge is balanced, so both sides hang over the bed; only the larger (here, either) side keeps fins.
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ cube_on_edge() }, print, {
        { "enable_support",             1 },
        { "support_type",               "fins" },
        { "layer_height",               0.2 },
        { "support_fin_lean_side_only", 1 },
    });
    const PrintObject &object = *print.objects().front();
    REQUIRE(! object.support_layers().empty());
    // The cube rests on an edge along X at the middle of its footprint in Y.
    double area;
    const double edge_y = area_centroid_y(object.layers().front()->lslices, area);
    size_t below = 0, above = 0;
    for (const SupportLayer *layer : object.support_layers())
        for (const ExPolygon &fin : layer->support_islands)
            ++ (fin.contour.centroid().y() < edge_y ? below : above);
    CAPTURE(below, above);
    CHECK(below + above > 0);
    CHECK(std::min(below, above) == 0);
}

static double support_area_at(const PrintObject &object, double z)
{
    for (const SupportLayer *layer : object.support_layers())
        if (layer->print_z > z)
            return area(layer->support_islands);
    return 0.;
}

TEST_CASE("Fin support with cross fins adds bracing across the main fins", "[SupportMaterial][Fins]")
{
    auto slice_table = [](double cross) {
        auto print = std::make_unique<Slic3r::Print>();
        Slic3r::Test::init_and_process_print({ support_table() }, *print, {
            { "enable_support",            1 },
            { "support_type",              "fins" },
            { "layer_height",              0.2 },
            { "support_fin_tine_spacing", 0 },
            { "support_fin_cross_spacing", cross },
        });
        return print;
    };
    const auto plain  = slice_table(0.);
    const auto braced = slice_table(6.);
    // Halfway up the stem, the scaffold covers more area than the parallel fins alone.
    CHECK(support_area_at(*braced->objects().front(), 5.) > support_area_at(*plain->objects().front(), 5.));
}

// Support layers carrying interface extrusions.
static size_t fin_interface_layer_count(const PrintObject &object)
{
    size_t count = 0;
    for (const SupportLayer *layer : object.support_layers()) {
        const ExtrusionEntityCollection flat = layer->support_fills.flatten();
        count += std::any_of(flat.entities.begin(), flat.entities.end(),
                             [](const ExtrusionEntity *e) { return e->role() == ExtrusionRole::erSupportMaterialInterface; });
    }
    return count;
}

TEST_CASE("Fin support prints interface layers under the object only when asked", "[SupportMaterial][Fins]")
{
    auto interface_layers = [](int layers) {
        Slic3r::Print print;
        Slic3r::Test::init_and_process_print({ support_table() }, print, {
            { "enable_support",               1 },
            { "support_type",                 "fins" },
            { "layer_height",                 0.2 },
            { "support_fin_interface_layers", layers },
        });
        return fin_interface_layer_count(*print.objects().front());
    };
    CHECK(interface_layers(0) == 0);
    // Two interface layers under the slab, above its Z gap.
    CHECK(interface_layers(2) >= 2);
}

TEST_CASE("Fin support tines are packed near the bed, then follow the tine spacing", "[SupportMaterial][Fins]")
{
    // The cube on its edge is 28 mm tall, sliced at 0.2 mm.
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({ cube_on_edge() }, print, model, {
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "support_fin_tine_spacing",   5 },
    });
    print.process();
    const PrintObject &object = *print.objects().front();
    const auto layers = object.layers();
    const std::vector<bool> rows = fin_tine_layers(object);
    std::vector<double> z;
    for (size_t i = 0; i < rows.size(); ++ i)
        if (rows[i])
            z.push_back(layers[i]->print_z);
    REQUIRE(z.size() >= 6);
    // The first tine sits just above the 1 mm foot; the gaps widen, then settle at 5 mm.
    CHECK(z.front() > 1.0);
    CHECK(z.front() < 1.5);
    CHECK(z[1] - z[0] < 1.5);
    CHECK(std::abs(z.back() - z[z.size() - 2] - 5.) < 0.25);
    // Without the extra rows near the bed, every gap is the tine spacing.
    const_cast<PrintObjectConfig&>(object.config()).support_fin_tine_base_rows.value = false;
    const std::vector<bool> plain = fin_tine_layers(object);
    CHECK(std::count(plain.begin(), plain.end(), true) < std::count(rows.begin(), rows.end(), true));
}

