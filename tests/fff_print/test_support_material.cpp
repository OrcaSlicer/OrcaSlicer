#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include "test_helpers.hpp" // get access to init_print, etc

using namespace Slic3r::Test;
using namespace Slic3r;

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

TEST_CASE("Conical build plate support reaches around lower geometry", "[SupportMaterial]")
{
    // A roof is connected to the build plate by a post at its left edge. A
    // shorter block below the middle of the roof leaves enough height for a
    // conical support to move sideways and reach the build plate around it.
    TriangleMesh model = make_cube(5., 20., 25.);
    TriangleMesh lower_block = make_cube(20., 20., 10.);
    lower_block.translate(10.f, 0.f, 0.f);
    model.merge(lower_block);
    TriangleMesh roof = make_cube(40., 20., 5.);
    roof.translate(0.f, 0.f, 25.f);
    model.merge(roof);

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

    const double everywhere_area = support_area_near_z(everywhere, 20.);
    const double buildplate_area = support_area_near_z(buildplate, 20.);
    REQUIRE(everywhere_area > 0.);
    REQUIRE(buildplate_area > 0.);
    REQUIRE(buildplate_area > 0.75 * everywhere_area);
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
