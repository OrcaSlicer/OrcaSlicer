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

SCENARIO("Extra bridging layers use correct fan speed for perimeters", "[FanSpeed][Bridge][ExtraBridge][Perimeter]")
{
    GIVEN("A bridge test mesh with extra bridge layers enabled")
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
            { "enable_extra_bridge_layer", "apply_to_all" },
            { "sparse_infill_density", 0 },
            { "top_shell_layers", 0 },
            { "bottom_shell_layers", 1 },
            { "thick_bridges", false }
        });

        WHEN("Inspecting perimeter roles by Z height")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("First bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            double extra_z = bridge_z + 0.2; // one layer_height above
            INFO("Extra bridge Z: " << extra_z);

            const auto roles_at_bridge = collect_perimeter_roles_at_z(print, bridge_z);
            const auto roles_at_extra = collect_perimeter_roles_at_z(print, extra_z);

            THEN("Perimeters on the first bridge layer are marked as bridge roles")
            {
                REQUIRE(contains_bridge_role(roles_at_bridge));
            }

            THEN("Perimeters on the extra bridge layer are also marked as bridge roles")
            {
                for (auto r : unique_roles(roles_at_extra)) {
                    INFO("Extra layer perimeter role: " << (int)r);
                }
                REQUIRE(contains_bridge_role(roles_at_extra));
            }
        }
    }
}
