#include "libslic3r/Point.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <limits>
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

static double support_top_z(const Print &print)
{
    double top_z = 0.;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        if (layer->has_extrusions())
            top_z = std::max(top_z, layer->print_z);
    return top_z;
}

static TriangleMesh rounded_cantilever(float bevel_inset = 0.f)
{
    // A plate with two rounded free corners, attached to a wall along its other edge.
    std::vector<Vec3f> vertices;
    for (const float z : {20.f, 20.4f, 22.f}) {
        const float inset = z == 20.f ? bevel_inset : 0.f;
        vertices.emplace_back(20.f, inset, z);
        vertices.emplace_back(20.f, 20.f - inset, z);
        for (const float cy : {4.f, 16.f})
            for (int i = 0; i < 64; ++i) {
                const double angle = 2 * M_PI * i / 64;
                vertices.emplace_back(4.f + float((4 - inset) * std::cos(angle)), cy + float((4 - inset) * std::sin(angle)), z);
            }
    }
    TriangleMesh plate(its_convex_hull(vertices));
    TriangleMesh wall = make_cube(2, 20, 22);
    wall.translate(20, 0, 0);
    plate.merge(wall);
    return plate;
}

TEST_CASE("Tree support leaves the top contact gap below bridge extrusions", "[SupportMaterial][Regression]")
{
    const auto style = GENERATE("default", "organic", "tree_slim", "tree_strong", "tree_hybrid");
    const bool thick_bridges = GENERATE(false, true);
    const bool independent_layers = GENERATE(false, true);
    const double nozzle_diameter = GENERATE(0.4, 0.6);
    const double bridge_flow = GENERATE(1.0, 0.8);
    const auto fixture = GENERATE(TestMesh::bridge, TestMesh::bridge_with_hole);
    CAPTURE(style, thick_bridges, independent_layers, nozzle_diameter, bridge_flow, fixture);

    Print print;
    init_and_process_print({ fixture }, print, {
        { "enable_support", true },
        { "support_type", "tree(auto)" },
        { "support_style", style },
        { "dont_support_bridges", false },
        { "max_bridge_length", 0.0 },
        { "support_on_build_plate_only", true },
        { "support_interface_top_layers", 3 },
        { "support_top_z_distance", 0.2 },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "nozzle_diameter", nozzle_diameter },
        { "bridge_flow", bridge_flow },
        { "bridge_line_width", 0.0 },
        { "thick_bridges", thick_bridges },
        { "independent_support_layer_height", independent_layers }
    });

    const PrintObject &object = *print.objects().front();
    bool checked_bridge = false;
    for (const Layer *layer : object.layers()) {
        for (const LayerRegion *region : layer->regions()) {
            const Polygons bridge_area = to_polygons(region->slices.filter_by_type(stBottomBridge));
            if (bridge_area.empty())
                continue;
            const double bridge_bottom_z = layer->print_z - region->bridging_flow(frSolidInfill, thick_bridges).height();
            double support_top_z = 0.;
            for (const SupportLayer *support_layer : object.support_layers()) {
                if (support_layer->print_z < layer->print_z &&
                    !intersection(support_layer->support_fills.polygons_covered_by_width(), bridge_area).empty())
                    support_top_z = std::max(support_top_z, support_layer->print_z);
            }
            REQUIRE(support_top_z > 0.);
            const double gap = bridge_bottom_z - support_top_z;
            CAPTURE(bridge_bottom_z, support_top_z, gap);
            REQUIRE(gap >= 0.2 - EPSILON);
            REQUIRE(gap < 0.4 + EPSILON);
            checked_bridge = true;
        }
    }
    REQUIRE(checked_bridge);
}

TEST_CASE("Tree support leaves the top gap below bridge extrusion footprints", "[SupportMaterial][Regression]")
{
    const auto style = GENERATE("default", "organic", "tree_slim", "tree_strong", "tree_hybrid");
    const int fixture = GENERATE(0, 1, 2, 3, 4);
    const bool independent_layers = GENERATE(false, true);
    const double max_bridge_length = GENERATE(0.0, 10.0);
    const bool enforced = GENERATE(false, true);
    CAPTURE(style, fixture, independent_layers, max_bridge_length, enforced);

    TriangleMesh model_mesh = fixture == 0 ? mesh(TestMesh::bridge) :
                              fixture == 1 ? mesh(TestMesh::bridge_with_hole) :
                              rounded_cantilever(fixture == 3 ? 0.4f : fixture == 4 ? 0.8f : 0.f);

    Print print;
    Model model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", true },
        { "support_type", "tree(auto)" },
        { "support_style", style },
        { "max_bridge_length", max_bridge_length },
        { "support_on_build_plate_only", true },
        { "support_interface_top_layers", 2 },
        { "support_top_z_distance", 0.2 },
        { "support_object_xy_distance", 0.3 },
        { "support_threshold_angle", 35 },
        { "tree_support_tip_diameter", 2.0 },
        { "tree_support_branch_diameter_organic", 5.0 },
        { "tree_support_top_rate", "100%" },
        { "support_line_width", 0.42 },
        { "support_interface_spacing", 0.0 },
        { "support_interface_pattern", "rectilinear_interlaced" },
        { "outer_wall_line_width", 0.42 },
        { "inner_wall_line_width", 0.44 },
        { "internal_solid_infill_line_width", 0.5 },
        { "wall_loops", 2 },
        { "wall_generator", "classic" },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "nozzle_diameter", 0.4 },
        { "bridge_flow", 1.0 },
        { "thick_bridges", true },
        { "independent_support_layer_height", independent_layers }
    });
    init_print({ model_mesh }, print, model, config);
    if (enforced) {
        model.objects.front()->add_volume(model_mesh)->set_type(ModelVolumeType::SUPPORT_ENFORCER);
        print.apply(model, config);
    }
    print.process();
    const PrintObject &object = *print.objects().front();
    bool checked_perimeter = false;
    bool checked_bridge = false;
    double closest_gap = std::numeric_limits<double>::infinity();
    for (const Layer *layer : object.layers()) {
        auto check_path = [&](const ExtrusionPath &path) {
            if (path.role() != erOverhangPerimeter && path.role() != erBridgeInfill)
                return;
            checked_perimeter |= path.role() == erOverhangPerimeter;
            checked_bridge |= path.role() == erBridgeInfill;
            const double bottom_z = layer->print_z - path.height;
            const Polygons footprint = path.polygons_covered_by_width(-SCALED_EPSILON);
            for (const SupportLayer *support : object.support_layers()) {
                if (support->print_z >= layer->print_z || support->print_z < bottom_z - 0.4 - EPSILON)
                    continue;
                const Polygons overlap = intersection(footprint, support->support_fills.polygons_covered_by_width(-SCALED_EPSILON));
                if (overlap.empty())
                    continue;
                const double gap = bottom_z - support->print_z;
                closest_gap = std::min(closest_gap, gap);
                CAPTURE(layer->print_z, path.height, bottom_z, support->print_z);
                REQUIRE(gap >= 0.2 - EPSILON);
            }
        };
        for (const LayerRegion *region : layer->regions()) {
            for (const ExtrusionEntityCollection *collection : { &region->perimeters, &region->fills }) {
                for (const ExtrusionEntity *entity : collection->flatten().entities) {
                    if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                        check_path(*path);
                    else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                        for (const ExtrusionPath &path : loop->paths)
                            check_path(path);
                    else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity))
                        for (const ExtrusionPath &path : multi->paths)
                            check_path(path);
                }
            }
        }
    }
    REQUIRE(checked_perimeter);
    REQUIRE(checked_bridge);
    REQUIRE(closest_gap < 0.4 + EPSILON);
}

TEST_CASE("Tree support keeps zero top contact distance when thick bridges are enabled", "[SupportMaterial][Regression]")
{
    const auto style = GENERATE("default", "organic", "tree_slim", "tree_strong", "tree_hybrid");
    const bool independent_layers = GENERATE(false, true);
    CAPTURE(style, independent_layers);

    Print thin;
    Print thick;
    for (Print *print : { &thin, &thick }) {
        init_and_process_print({ TestMesh::bridge }, *print, {
            { "enable_support", true },
            { "support_type", "tree(auto)" },
            { "support_style", style },
            { "max_bridge_length", 0.0 },
            { "support_on_build_plate_only", true },
            { "support_interface_top_layers", 3 },
            { "support_top_z_distance", 0.0 },
            { "layer_height", 0.2 },
            { "initial_layer_print_height", 0.2 },
            { "nozzle_diameter", 0.4 },
            { "thick_bridges", print == &thick },
            { "independent_support_layer_height", independent_layers }
        });
    }
    // The bridge fixture's underside is 5 mm above the plate. Zero-gap support
    // reaches that surface with either bridge extrusion setting.
    for (const Print *print : { &thin, &thick }) {
        REQUIRE_THAT(support_top_z(*print), Catch::Matchers::WithinAbs(5.0, EPSILON));
    }
}

TEST_CASE("Changing bridge flow recalculates tree support clearance", "[SupportMaterial][Regression]")
{
    const auto style = GENERATE("default", "organic", "tree_slim", "tree_strong", "tree_hybrid");
    const bool independent_layers = GENERATE(false, true);
    CAPTURE(style, independent_layers);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", true },
        { "support_type", "tree(auto)" },
        { "support_style", style },
        { "max_bridge_length", 0.0 },
        { "support_on_build_plate_only", true },
        { "support_interface_top_layers", 3 },
        { "support_top_z_distance", 0.2 },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "nozzle_diameter", 0.6 },
        { "bridge_flow", 1.0 },
        { "bridge_line_width", 0.0 },
        { "thick_bridges", true },
        { "independent_support_layer_height", independent_layers }
    });
    Print incremental;
    Model model;
    init_print({ TestMesh::bridge }, incremental, model, config);
    incremental.process();
    const double old_top_z = support_top_z(incremental);

    config.set_deserialize_strict({ { "bridge_flow", 0.25 } });
    incremental.apply(model, config);
    incremental.process();

    Print fresh;
    init_and_process_print({ TestMesh::bridge }, fresh, config);
    REQUIRE(support_top_z(incremental) > old_top_z + EPSILON);
    REQUIRE_THAT(support_top_z(incremental), Catch::Matchers::WithinAbs(support_top_z(fresh), EPSILON));
}


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

