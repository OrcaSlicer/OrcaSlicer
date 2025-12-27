#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include "test_data.hpp" // get access to init_print, etc

using namespace Slic3r::Test;
using namespace Slic3r;

TEST_CASE("SupportMaterial: Three raft layers created", "[SupportMaterial][.]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, {
		{ "support_material", 1 },
		{ "raft_layers",      3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

SCENARIO("SupportMaterial: support_layers_z and contact_distance", "[SupportMaterial][.]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));
//    mesh.write_binary("d:\\temp\\cube_with_hole.stl");

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok, bool &top_spacing_ok)
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

#if 0
		double expected_top_spacing = print.default_object_config().layer_height + print.config().nozzle_diameter.get_at(0);
		bool wrong_top_spacing = 0;
        std::vector<coordf_t> top_z { 1.1 };
		for (coordf_t top_z_el : top_z) {
			// find layer index of this top surface.
			size_t layer_id = -1;
			for (size_t i = 0; i < support_z.size(); ++ i) {
				if (abs(support_z[i] - top_z_el) < EPSILON) {
					layer_id = i;
					i = static_cast<int>(support_z.size());
				}
			}

			// check that first support layer above this top surface (or the next one) is spaced with nozzle diameter
			if (abs(support_z[layer_id + 1] - support_z[layer_id] - expected_top_spacing) > EPSILON && 
				abs(support_z[layer_id + 2] - support_z[layer_id] - expected_top_spacing) > EPSILON) {
				wrong_top_spacing = 1;
			}
		}
		d = ! wrong_top_spacing;
#else
		top_spacing_ok = true;
#endif
	};

    GIVEN("A print object having one modelObject") {
        WHEN("First layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "support_material",	1 },
				{ "layer_height",		0.2 },
				{ "first_layer_height", 0.4 },
                { "dont_support_bridges", false },
			});
			bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
        WHEN("Layer height = 0.2 and, first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "support_material",	1 },
				{ "layer_height",		0.2 },
				{ "first_layer_height", 0.3 },
                { "dont_support_bridges", false },
            });
            bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
        WHEN("Layer height = nozzle_diameter[0]") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "support_material",	1 },
				{ "layer_height",		0.2 },
				{ "first_layer_height", 0.3 },
                { "dont_support_bridges", false },
            });
            bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
    }
}

#if 0
// Test 8.
TEST_CASE("SupportMaterial: forced support is generated", "[SupportMaterial]")
{
    // Create a mesh & modelObject.
    TriangleMesh mesh = TriangleMesh::make_cube(20, 20, 20);

    Model model = Model();
    ModelObject *object = model.add_object();
    object->add_volume(mesh);
    model.add_default_instances();
    model.align_instances_to_origin();

    Print print = Print();

    std::vector<coordf_t> contact_z = {1.9};
    std::vector<coordf_t> top_z = {1.1};
    print.default_object_config.support_material_enforce_layers = 100;
    print.default_object_config.support_material = 0;
    print.default_object_config.layer_height = 0.2;
    print.default_object_config.set_deserialize("first_layer_height", "0.3");

    print.add_model_object(model.objects[0]);
    print.objects.front()->_slice();

    SupportMaterial *support = print.objects.front()->_support_material();
    auto support_z = support->support_layers_z(contact_z, top_z, print.default_object_config.layer_height);

    bool check = true;
    for (size_t i = 1; i < support_z.size(); i++) {
        if (support_z[i] - support_z[i - 1] <= 0)
            check = false;
    }

    REQUIRE(check == true);
}

// TODO
bool test_6_checks(Print& print)
{
	bool has_bridge_speed = true;

	// Pre-Processing.
	PrintObject* print_object = print.objects.front();
	print_object->infill();
	SupportMaterial* support_material = print.objects.front()->_support_material();
	support_material->generate(print_object);
	// TODO but not needed in test 6 (make brims and make skirts).

	// Exporting gcode.
	// TODO validation found in Simple.pm


	return has_bridge_speed;
}

// Test 6.
SCENARIO("SupportMaterial: Checking bridge speed", "[SupportMaterial]")
{
    GIVEN("Print object") {
        // Create a mesh & modelObject.
        TriangleMesh mesh = TriangleMesh::make_cube(20, 20, 20);

        Model model = Model();
        ModelObject *object = model.add_object();
        object->add_volume(mesh);
        model.add_default_instances();
        model.align_instances_to_origin();

        Print print = Print();
        print.config.brim_width = 0;
        print.config.skirts = 0;
        print.config.skirts = 0;
        print.default_object_config.support_material = 1;
        print.default_region_config.top_solid_layers = 0; // so that we don't have the internal bridge over infill.
        print.default_region_config.bridge_speed = 99;
        print.config.cooling = 0;
        print.config.set_deserialize("first_layer_speed", "100%");

        WHEN("support_material_contact_distance = 0.2") {
            print.default_object_config.support_material_contact_distance = 0.2;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is used.
        }

        WHEN("support_material_contact_distance = 0") {
            print.default_object_config.support_material_contact_distance = 0;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is not used.
        }

        WHEN("support_material_contact_distance = 0.2 & raft_layers = 5") {
            print.default_object_config.support_material_contact_distance = 0.2;
            print.default_object_config.raft_layers = 5;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is used.
        }

        WHEN("support_material_contact_distance = 0 & raft_layers = 5") {
            print.default_object_config.support_material_contact_distance = 0;
            print.default_object_config.raft_layers = 5;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);

            REQUIRE(check == true); // bridge speed is not used.
        }
    }
}

#endif
