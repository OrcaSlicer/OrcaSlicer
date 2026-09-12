#include <catch2/catch_all.hpp>

#include <tuple>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "test_helpers.hpp" // get access to init_print, etc
#include "libslic3r/Support/SupportParameters.hpp"

// Not self-contained: its inline constructor uses PrintObject, PrintRegion, SlicingParameters and
// Geometry, so it must follow the headers (pulled in via test_helpers.hpp) that define them.
#include "libslic3r/Support/SupportParameters.hpp"

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

TEST_CASE("Conical support narrows and subdivides wide areas toward the build plate", "[SupportMaterial]")
{
    // Model the same broad slab on a narrow central pillar used for manual
    // verification. Grid support used to erase each small per-layer offset.
    TriangleMesh pedestal = make_cube(8.2, 30., 62.6);
    pedestal.translate(20.2f, 0.f, 0.f);
    TriangleMesh overhang = make_cube(48.6, 30., 7.4);
    overhang.translate(0.f, 0.f, 62.6f);
    pedestal.merge(overhang);

    auto support_layer_near_z = [](const Print &print, double print_z) {
        const SupportLayer *nearest = nullptr;
        for (const SupportLayer *layer : print.objects().front()->support_layers())
            if (! layer->support_islands.empty() &&
                (nearest == nullptr || std::abs(layer->print_z - print_z) < std::abs(nearest->print_z - print_z)))
                nearest = layer;
        return nearest;
    };

    const std::initializer_list<ConfigBase::SetDeserializeItem> common_config {
        { "enable_support",                     1 },
        { "layer_height",                       0.2 },
        { "support_on_build_plate_only",        1 },
        { "support_remove_small_overhang",      0 },
        { "support_conical_angle",              30 },
        { "support_conical_min_width",          5 },
        { "support_conical_max_column_width",   0 }
    };

    DynamicPrintConfig straight_config = DynamicPrintConfig::full_print_config();
    straight_config.set_deserialize_strict(common_config);
    straight_config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(smsGrid));
    Print straight;
    init_and_process_print({ pedestal }, straight, straight_config);

    DynamicPrintConfig conical_config = straight_config;
    conical_config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(smsConical));
    Print conical;
    init_and_process_print({ pedestal }, conical, conical_config);
    REQUIRE(straight.objects().front()->config().support_style.value == smsGrid);
    REQUIRE(conical.objects().front()->config().support_style.value == smsConical);
    REQUIRE(SupportParameters(*conical.objects().front()).support_style == smsSnug);

    DynamicPrintConfig subdivided_config = conical_config;
    // Each original support region is wider than this limit and should become
    // multiple independently tapered columns.
    subdivided_config.set_key_value("support_conical_max_column_width", new ConfigOptionFloat(12));
    Print subdivided;
    init_and_process_print({ pedestal }, subdivided, subdivided_config);

    DynamicPrintConfig inactive_limit_config = conical_config;
    inactive_limit_config.set_key_value("support_conical_max_column_width", new ConfigOptionFloat(100));
    Print inactive_limit;
    init_and_process_print({ pedestal }, inactive_limit, inactive_limit_config);

    const SupportLayer *straight_low = support_layer_near_z(straight, 2.);
    const SupportLayer *conical_low  = support_layer_near_z(conical, 2.);
    const SupportLayer *conical_high = support_layer_near_z(conical, 58.);
    const SupportLayer *subdivided_low = support_layer_near_z(subdivided, 2.);
    const SupportLayer *subdivided_contact = support_layer_near_z(subdivided, 62.);
    const SupportLayer *conical_contact = support_layer_near_z(conical, 62.);
    const SupportLayer *inactive_limit_low = support_layer_near_z(inactive_limit, 2.);
    REQUIRE(straight_low != nullptr);
    REQUIRE(conical_low != nullptr);
    REQUIRE(conical_high != nullptr);
    REQUIRE(subdivided_low != nullptr);
    REQUIRE(subdivided_contact != nullptr);
    REQUIRE(conical_contact != nullptr);
    REQUIRE(inactive_limit_low != nullptr);

    const double straight_area_low = area(straight_low->support_islands);
    const double conical_area_low  = area(conical_low->support_islands);
    const double conical_area_high = area(conical_high->support_islands);
    REQUIRE(straight_area_low > 0.);
    REQUIRE(conical_area_low > 0.);
    REQUIRE(conical_area_high > 0.);
    REQUIRE(conical_area_low < 0.75 * straight_area_low);
    REQUIRE(conical_area_low < conical_area_high);
    REQUIRE(unscaled(unscaled(conical_area_low)) < 500.);
    REQUIRE(conical_low->support_islands.size() == 2);
    REQUIRE(subdivided_low->support_islands.size() > conical_low->support_islands.size());
    REQUIRE(subdivided_contact->support_islands.size() == conical_contact->support_islands.size());
    REQUIRE_THAT(area(subdivided_contact->support_islands),
        Catch::Matchers::WithinRel(area(conical_contact->support_islands), 0.01));
    REQUIRE(inactive_limit_low->support_islands.size() == conical_low->support_islands.size());
    REQUIRE_THAT(area(inactive_limit_low->support_islands),
        Catch::Matchers::WithinRel(conical_area_low, 0.001));

    size_t sampled_layers = 0;
    size_t changing_layers = 0;
    double previous_area = 0.;
    for (const SupportLayer *layer : conical.objects().front()->support_layers()) {
        if (layer->support_islands.empty() || layer->print_z < 48. || layer->print_z > 58.)
            continue;
        const double current_area = area(layer->support_islands);
        if (sampled_layers > 0 && std::abs(current_area - previous_area) > EPSILON)
            ++ changing_layers;
        previous_area = current_area;
        ++ sampled_layers;
    }
    REQUIRE(sampled_layers > 20);
    REQUIRE(changing_layers > 0.8 * (sampled_layers - 1));

    double previous_top_area = 0.;
    for (const SupportLayer *layer : conical.objects().front()->support_layers()) {
        if (layer->support_islands.empty() || layer->print_z < 55. || layer->print_z > 63.)
            continue;
        const double current_area = area(layer->support_islands);
        if (previous_top_area > 0.)
            REQUIRE(current_area < 1.1 * previous_top_area);
        previous_top_area = current_area;
    }
}

TEST_CASE("Zero-angle conical support matches snug support", "[SupportMaterial]")
{
    TriangleMesh model = make_cube(4., 20., 20.);
    TriangleMesh roof = make_cube(20., 20., 4.);
    roof.translate(0.f, 0.f, 20.f);
    model.merge(roof);

    auto support_signature = [](const Print &print) {
        std::vector<std::tuple<double, size_t, double>> signature;
        for (const SupportLayer *layer : print.objects().front()->support_layers()) {
            if (! layer->support_islands.empty())
                signature.emplace_back(layer->print_z, layer->support_islands.size(), area(layer->support_islands));
        }
        return signature;
    };

    DynamicPrintConfig snug_config = DynamicPrintConfig::full_print_config();
    snug_config.set_deserialize_strict({
        { "enable_support",                     1 },
        { "layer_height",                       0.2 },
        { "support_on_build_plate_only",        1 },
        { "support_remove_small_overhang",      0 },
        { "support_threshold_angle",            45 },
        { "support_interface_top_layers",       3 },
        { "support_style",                      "snug" },
        { "support_conical_angle",              0 },
        { "support_conical_min_width",          5 },
        { "support_conical_max_column_width",   0 }
    });
    Print snug;
    init_and_process_print({ model }, snug, snug_config);

    DynamicPrintConfig conical_config = snug_config;
    conical_config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(smsConical));
    Print conical;
    init_and_process_print({ model }, conical, conical_config);

    const auto snug_signature = support_signature(snug);
    const auto conical_signature = support_signature(conical);
    REQUIRE(conical_signature.size() == snug_signature.size());
    for (size_t i = 0; i < snug_signature.size(); ++ i) {
        REQUIRE_THAT(std::get<0>(conical_signature[i]), Catch::Matchers::WithinAbs(std::get<0>(snug_signature[i]), EPSILON));
        REQUIRE(std::get<1>(conical_signature[i]) == std::get<1>(snug_signature[i]));
        REQUIRE_THAT(std::get<2>(conical_signature[i]), Catch::Matchers::WithinRel(std::get<2>(snug_signature[i]), 0.001));
    }
}

TEST_CASE("Conical support preserves its minimum width across a large layer offset", "[SupportMaterial]")
{
    TriangleMesh model = make_cube(2., 10., 10.);
    TriangleMesh roof = make_cube(12., 10., 2.);
    roof.translate(0.f, 0.f, 10.f);
    model.merge(roof);

    Print print;
    init_and_process_print({ model }, print, {
        { "enable_support",                     1 },
        { "layer_height",                       0.2 },
        { "support_on_build_plate_only",        1 },
        { "support_remove_small_overhang",      0 },
        { "support_threshold_angle",            45 },
        { "support_style",                      "conical" },
        { "support_conical_angle",              89 },
        { "support_conical_min_width",          5 },
        { "support_conical_max_column_width",   0 }
    });

    const SupportLayer *middle = nullptr;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        if (! layer->support_islands.empty() &&
            (middle == nullptr || std::abs(layer->print_z - 5.) < std::abs(middle->print_z - 5.)))
            middle = layer;
    REQUIRE(middle != nullptr);
    REQUIRE(unscaled(unscaled(area(middle->support_islands))) >= 25.);
}

TEST_CASE("Conical column subdivision is bounded for tiny configured widths", "[SupportMaterial]")
{
    TriangleMesh model = make_cube(2., 10., 10.);
    TriangleMesh roof = make_cube(12., 10., 2.);
    roof.translate(0.f, 0.f, 10.f);
    model.merge(roof);

    Print print;
    init_and_process_print({ model }, print, {
        { "enable_support",                     1 },
        { "layer_height",                       0.2 },
        { "support_on_build_plate_only",        1 },
        { "support_remove_small_overhang",      0 },
        { "support_threshold_angle",            45 },
        { "support_style",                      "conical" },
        { "support_conical_angle",              30 },
        { "support_conical_min_width",          0 },
        { "support_conical_max_column_width",   0.01 }
    });

    bool generated_support = false;
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        generated_support |= ! layer->support_islands.empty();
        REQUIRE(layer->support_islands.size() <= 4096);
    }
    REQUIRE(generated_support);
}

TEST_CASE("Conical build plate support does not start above blocking geometry", "[SupportMaterial]")
{
    // The upper slab is entirely inside the vertical shadow of the lower slab.
    // Conical tapering must not reinterpret the enclosed gap as a path to the
    // build plate and leave disconnected support fragments above the lower slab.
    TriangleMesh model = make_cube(40., 30., 5.);
    TriangleMesh upper_slab = make_cube(20., 10., 5.);
    upper_slab.translate(10.f, 10.f, 35.f);
    model.merge(upper_slab);

    auto support_area_near_z = [](const Print &print, double print_z) {
        const SupportLayer *nearest = nullptr;
        for (const SupportLayer *layer : print.objects().front()->support_layers())
            if (! layer->support_islands.empty() &&
                (nearest == nullptr || std::abs(layer->print_z - print_z) < std::abs(nearest->print_z - print_z)))
                nearest = layer;
        return nearest == nullptr ? 0. : area(nearest->support_islands);
    };

    DynamicPrintConfig everywhere_config = DynamicPrintConfig::full_print_config();
    everywhere_config.set_deserialize_strict({
        { "enable_support",                     1 },
        { "layer_height",                       0.2 },
        { "support_on_build_plate_only",        0 },
        { "support_remove_small_overhang",      0 },
        { "support_threshold_angle",            45 },
        { "support_style",                      "conical" },
        { "support_conical_angle",              30 },
        { "support_conical_min_width",          5 },
        { "support_conical_max_column_width",   0 }
    });
    Print everywhere;
    init_and_process_print({ model }, everywhere, everywhere_config);

    DynamicPrintConfig buildplate_config = everywhere_config;
    buildplate_config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    Print buildplate;
    init_and_process_print({ model }, buildplate, buildplate_config);

    REQUIRE(support_area_near_z(everywhere, 20.) > 0.);
    REQUIRE(support_area_near_z(buildplate, 20.) == 0.);
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

