#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"

#include "test_helpers.hpp" // get access to init_print, etc

using namespace Slic3r::Test;
using namespace Slic3r;
using Catch::Matchers::WithinAbs;

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

// A "Print as Support" volume must (a) seed support material even where it never overlaps
// the main part's own footprint (a freestanding scaffold), and (b) contribute to the
// object's own height, since it may rise above the main part's own geometry entirely.
TEST_CASE("Support mesh volume seeds support independent of the main object's footprint and height", "[SupportMaterial]")
{
    // Main part: a 10mm cube at the origin.
    // Support mesh: a disconnected, taller column (10x10x30mm) placed 30mm away in X, so
    // its footprint never overlaps the main part's footprint at any layer.
    Slic3r::Model model;
    ModelObject *object = model.add_object();
    object->name = "support_mesh_test.stl";
    object->add_volume(make_cube(10, 10, 10))->set_type(ModelVolumeType::MODEL_PART);
    ModelVolume *support = object->add_volume(make_cube(10, 10, 30));
    support->set_type(ModelVolumeType::SUPPORT_MESH);
    support->translate(30, 0, 0);
    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", "1" },
        { "layer_height",   "0.2" },
    });

    Slic3r::Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    const PrintObject *print_object = print.objects().front();

    // Regression test: the object's own snug height must include the taller, disconnected
    // support mesh volume, or layers above the main part's own 10mm height would never be
    // generated (ModelObject::update_min_max_z / raw_bounding_box must consider SUPPORT_MESH
    // volumes, not just MODEL_PART).
    REQUIRE(!print_object->layers().empty());
    REQUIRE_THAT(print_object->layers().back()->print_z, WithinAbs(30.0, 1.0));

    // Support material must be generated, reaching up to the support mesh's own height, even
    // though the support mesh never overlaps the main part's footprint.
    REQUIRE(!print_object->support_layers().empty());
    REQUIRE_THAT(print_object->support_layers().back()->print_z, WithinAbs(30.0, 1.0));

    bool any_support_fill = false;
    for (const SupportLayer *sl : print_object->support_layers())
        if (sl->has_extrusions()) { any_support_fill = true; break; }
    REQUIRE(any_support_fill);
}
