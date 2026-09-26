#include <catch2/catch_all.hpp>
#include <set>

#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"
#include "test_bridge_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using namespace Slic3r::Test::BridgeHelpers;

SCENARIO("Bridge outer perimeters are treated as bridges for fan control", "[FanSpeed][Bridge][Perimeter]")
{
    GIVEN("A bridge test mesh with bridge fan speeds configured")
    {
        Slic3r::Print print;
        Slic3r::Model model;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height", 0.2 },
            { "initial_layer_print_height", 0.2 },
            { "gcode_comments", true },
            { "cooling", true },
            { "enable_overhang_bridge_fan", true },
            { "overhang_fan_speed", 100 },
            { "sparse_infill_density", 0 },
            { "top_shell_layers", 0 },
            { "bottom_shell_layers", 1 },
        });

        WHEN("Inspecting generated perimeters")
        {
            bool found_bridge_role_in_perimeters = false;
            std::set<ExtrusionRole> seen_roles;
            for (const PrintObject *obj : print.objects()) {
                for (const Layer *layer : obj->layers()) {
                    for (const LayerRegion *region : layer->regions()) {
                        std::vector<ExtrusionRole> roles;
                        collect_roles_from_entity(region->perimeters, roles);
                        for (auto r : roles) {
                            seen_roles.insert(r);
                            if (r == erBridgeInfill || r == erOverhangPerimeter || r == erBridgePerimeter) {
                                found_bridge_role_in_perimeters = true;
                                break;
                            }
                        }
                    }
                    if (found_bridge_role_in_perimeters)
                        break;
                }
                if (found_bridge_role_in_perimeters)
                    break;
            }

            THEN("Some perimeter paths are marked as bridge or overhang role")
            {
                for (auto r : seen_roles) {
                    INFO("Seen perimeter role: " << (int)r);
                }
                REQUIRE(found_bridge_role_in_perimeters);
            }
        }
        WHEN("Inspecting bridge layer perimeters by Z height")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("First bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            const std::vector<ExtrusionRole> roles_at_bridge = collect_perimeter_roles_at_z(print, bridge_z);
            THEN("Bridge layer perimeters include a bridge role")
            {
                REQUIRE(contains_bridge_role(roles_at_bridge));
            }
        }
    }
}
