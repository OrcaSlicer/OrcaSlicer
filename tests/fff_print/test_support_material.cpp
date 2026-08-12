#include <catch2/catch_all.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

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

// Lines of the first ironing block: everything between ";TYPE:Ironing" and the next ";TYPE:"
// (or ";LAYER_CHANGE"), plus the `before` lines preceding the tag.
static std::pair<std::vector<std::string>, std::vector<std::string>> ironing_block(const std::string &gcode, size_t before = 20)
{
    std::vector<std::string> lines;
    std::istringstream stream(gcode);
    std::string line;
    while (std::getline(stream, line))
        lines.emplace_back(line);
    auto tag = std::find_if(lines.begin(), lines.end(), [](const std::string &l) { return l.rfind(";TYPE:Ironing", 0) == 0; });
    REQUIRE(tag != lines.end());
    auto block_end = std::find_if(tag + 1, lines.end(), [](const std::string &l) {
        return l.rfind(";TYPE:", 0) == 0 || l.rfind(";LAYER_CHANGE", 0) == 0;
    });
    auto first = tag - std::min<size_t>(before, tag - lines.begin());
    return { std::vector<std::string>(first, tag), std::vector<std::string>(tag + 1, block_end) };
}

// A zero flow support ironing pass extrudes nothing, so the interface flow has no cross section
// at all — which used to abort slicing — and the pass retracts to stop the nozzle oozing onto
// the surface it just smoothed.
TEST_CASE("Zero flow support ironing retracts around the ironing block", "[SupportMaterial][Ironing]")
{
    auto sliced = [](int ironing_flow) {
        return slice({ TestMesh::overhang }, {
            { "enable_support",                  1 },
            { "support_interface_top_layers",    2 },
            { "support_ironing",                 1 },
            { "support_ironing_flow",            ironing_flow },
            { "support_ironing_retract",         5 },
            { "support_ironing_unretract_extra", 0.5 },
            { "use_relative_e_distances",        1 },
        });
    };

    SECTION("zero flow") {
        const auto [before, block] = ironing_block(sliced(0));

        // The 5mm ironing retract precedes the block; the unretract (5 + 0.5 extra) closes it.
        const bool retract_before = std::any_of(before.begin(), before.end(),
            [](const std::string &l) { return l.rfind("G1 E-5 ", 0) == 0; });
        REQUIRE(retract_before);
        const bool unretract_in_block = std::any_of(block.begin(), block.end(),
            [](const std::string &l) { return l.rfind("G1 E5.5 ", 0) == 0; });
        REQUIRE(unretract_in_block);

        // Ironing strokes carry an explicit E word even at zero flow — the preview
        // relies on it to tell strokes apart from travel moves.
        const bool stroke_with_e = std::any_of(block.begin(), block.end(),
            [](const std::string &l) { return l.rfind("G1 X", 0) == 0 && l.find(" E") != std::string::npos; });
        REQUIRE(stroke_with_e);
    }

    SECTION("non-zero flow leaves the pass untouched") {
        const auto [before, block] = ironing_block(sliced(10));

        const bool retract_before = std::any_of(before.begin(), before.end(),
            [](const std::string &l) { return l.rfind("G1 E-5 ", 0) == 0; });
        REQUIRE_FALSE(retract_before);
        const bool unretract_in_block = std::any_of(block.begin(), block.end(),
            [](const std::string &l) { return l.rfind("G1 E5.5 ", 0) == 0; });
        REQUIRE_FALSE(unretract_in_block);
    }
}
