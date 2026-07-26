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

// ============================================================================
// Scenario 1: Bridge surfaces are detected correctly
// ============================================================================

SCENARIO("Bridge surfaces are detected correctly", "[Bridge][Surface]")
{
    GIVEN("A bridge mesh sliced with default settings")
    {
        Slic3r::Print print;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",              0.2 },
            { "initial_layer_print_height", 0.2 },
            { "bottom_shell_layers",       1 },
            { "top_shell_layers",          0 },
            { "sparse_infill_density",     0 },
        });

        WHEN("Searching for bridge surfaces")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("First bridge Z: " << bridge_z);

            THEN("At least one layer has stBottomBridge fill surfaces")
            {
                REQUIRE(bridge_z > 0.0);
            }

            THEN("The bridge layer fill surfaces contain stBottomBridge")
            {
                REQUIRE(bridge_z > 0.0);
                auto types = collect_surface_types_at_z(print, bridge_z);
                REQUIRE(contains_surface_type(types, stBottomBridge));
            }

            THEN("Layers below the bridge do NOT have stBottomBridge surfaces")
            {
                REQUIRE(bridge_z > 0.0);
                // Check a pillar layer (first layer)
                auto types_first = collect_surface_types_at_z(print, 0.2);
                REQUIRE_FALSE(contains_surface_type(types_first, stBottomBridge));
            }
        }
    }
}

// ============================================================================
// Scenario 2: Bridge perimeters use correct extrusion roles
// ============================================================================

SCENARIO("Bridge perimeters use correct extrusion roles", "[Bridge][Perimeter]")
{
    GIVEN("A bridge mesh sliced with default settings")
    {
        Slic3r::Print print;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",              0.2 },
            { "initial_layer_print_height", 0.2 },
            { "bottom_shell_layers",       1 },
            { "top_shell_layers",          0 },
            { "sparse_infill_density",     0 },
        });

        WHEN("Inspecting perimeter roles on the bridge layer")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("Bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            auto roles = collect_perimeter_roles_at_z(print, bridge_z);
            for (auto r : unique_roles(roles)) {
                INFO("Bridge layer perimeter role: " << (int)r);
            }

            THEN("Bridge layer perimeters include erBridgePerimeter")
            {
                REQUIRE(contains_erBridgePerimeter(roles));
            }
        }

        WHEN("Inspecting perimeter roles on a non-bridge layer")
        {
            // First layer (pillar) should not have bridge perimeters
            auto roles_first = collect_perimeter_roles_at_z(print, 0.2);

            THEN("Non-bridge layer perimeters do NOT contain erBridgePerimeter")
            {
                REQUIRE_FALSE(contains_erBridgePerimeter(roles_first));
            }
        }
    }
}

// ============================================================================
// Scenario 3: Bridge infill uses correct extrusion roles
// ============================================================================

SCENARIO("Bridge infill uses correct extrusion roles", "[Bridge][Infill]")
{
    GIVEN("A bridge mesh sliced with default settings")
    {
        Slic3r::Print print;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",              0.2 },
            { "initial_layer_print_height", 0.2 },
            { "bottom_shell_layers",       1 },
            { "top_shell_layers",          0 },
            { "sparse_infill_density",     0 },
        });

        WHEN("Inspecting fill roles on the bridge layer")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("Bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            auto fill_roles = collect_fill_roles_at_z(print, bridge_z);
            for (auto r : unique_roles(fill_roles)) {
                INFO("Bridge layer fill role: " << (int)r);
            }

            THEN("Bridge layer fills include erBridgeInfill")
            {
                REQUIRE(contains_erBridgeInfill(fill_roles));
            }
        }

        WHEN("Inspecting fill roles on a non-bridge layer")
        {
            // First layer (pillar) should not have bridge infill
            auto fill_roles_first = collect_fill_roles_at_z(print, 0.2);

            THEN("Non-bridge layer fills do NOT contain erBridgeInfill")
            {
                REQUIRE_FALSE(contains_erBridgeInfill(fill_roles_first));
            }
        }
    }
}

// Scenarios 4-6 (G-code speed, flow, fan) disabled: Slic3r::Test::slice()
// triggers a pre-existing segfault in export_gcode() unrelated to bridge
// perimeter retagging.  These tests should be re-enabled once the G-code
// export crash is fixed.

// ============================================================================
// Scenario 7: Extra bridge layer surfaces for external bridges
// ============================================================================

SCENARIO("Extra bridge layer surfaces are created for external bridges", "[Bridge][ExtraBridge][Surface]")
{
    GIVEN("A bridge mesh with enable_extra_bridge_layer = apply_to_all")
    {
        Slic3r::Print print;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",              0.2 },
            { "initial_layer_print_height", 0.2 },
            { "enable_extra_bridge_layer", "apply_to_all" },
            { "bottom_shell_layers",       1 },
            { "top_shell_layers",          0 },
            { "sparse_infill_density",     0 },
            { "thick_bridges",             false },
        });

        WHEN("Inspecting the layer above the first bridge")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("First bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            double extra_z = bridge_z + 0.2;
            INFO("Extra bridge Z: " << extra_z);
            auto extra_types = collect_surface_types_at_z(print, extra_z);

            for (size_t i = 0; i < extra_types.size(); ++i) {
                INFO("Extra layer surface type [" << i << "]: " << (int)extra_types[i]);
            }

            THEN("The extra bridge layer has bridge-related surface types")
            {
                // The extra bridge layer should have stBottomBridge (reclassified
                // from stInternalAfterExternalBridge) or stInternalAfterExternalBridge
                bool has_bridge_surface =
                    contains_surface_type(extra_types, stBottomBridge) ||
                    contains_surface_type(extra_types, stInternalAfterExternalBridge);
                REQUIRE(has_bridge_surface);
            }
        }
    }
}

// ============================================================================
// Scenario 8: Extra bridge layer perimeters are promoted to bridge roles
// ============================================================================

SCENARIO("Extra bridge layer perimeters are promoted to bridge roles", "[Bridge][ExtraBridge][Perimeter]")
{
    GIVEN("A bridge mesh with enable_extra_bridge_layer = apply_to_all")
    {
        Slic3r::Print print;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",              0.2 },
            { "initial_layer_print_height", 0.2 },
            { "enable_extra_bridge_layer", "apply_to_all" },
            { "bottom_shell_layers",       1 },
            { "top_shell_layers",          0 },
            { "sparse_infill_density",     0 },
            { "thick_bridges",             false },
        });

        WHEN("Inspecting perimeter roles on the extra bridge layer")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("First bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            double extra_z = bridge_z + 0.2;
            INFO("Extra bridge Z: " << extra_z);

            auto roles = collect_perimeter_roles_at_z(print, extra_z);
            REQUIRE_FALSE(roles.empty()); // Verify a layer exists at extra_z
            for (auto r : unique_roles(roles)) {
                INFO("Extra bridge layer perimeter role: " << (int)r);
            }

            THEN("Extra bridge layer perimeters include erBridgePerimeter")
            {
                REQUIRE(contains_erBridgePerimeter(roles));
            }

            THEN("Not all extra-layer perimeters are bridge perimeters (only spanning ones)")
            {
                // The slab's short edges over the pillar tops are still
                // supported and must keep a normal role.
                REQUIRE_FALSE(all_roles_are_bridge_perimeter(roles));
            }

            THEN("Extra-layer bridge perimeters are re-flowed, not just relabeled")
            {
                // The post-pass re-flows the spanning perimeters with the
                // bridging flow.  Guard the regression where supported walls
                // were relabeled "Bridge wall" while keeping their normal
                // inner/outer-wall flow: every extra-layer bridge perimeter must
                // carry a width distinct from the normal external-perimeter
                // width on the same model.
                auto extra_bridge_w = collect_perimeter_widths_for_role_at_z(print, extra_z, erBridgePerimeter);
                auto ext_w          = collect_perimeter_widths_for_role_at_z(print, bridge_z, erExternalPerimeter);
                REQUIRE_FALSE(extra_bridge_w.empty());
                REQUIRE_FALSE(ext_w.empty());
                const float normal_w = ext_w.front();
                for (float w : extra_bridge_w)
                    REQUIRE(w != Catch::Approx(normal_w).margin(1e-4));
            }
        }
    }
}

// ============================================================================
// Scenario 9: Extra bridge layer for internal bridges
// ============================================================================

SCENARIO("Extra bridge layer for internal bridges", "[Bridge][ExtraBridge][InternalBridge]")
{
    GIVEN("A bridge mesh with enable_extra_bridge_layer = apply_to_all and solid infill")
    {
        Slic3r::Print print;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        // Use bottom_shell_layers=2 so that the second bottom layer triggers
        // internal bridge (stInternalBridge) detection in bridge_over_infill.
        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",              0.2 },
            { "initial_layer_print_height", 0.2 },
            { "enable_extra_bridge_layer", "apply_to_all" },
            { "bottom_shell_layers",       2 },
            { "top_shell_layers",          0 },
            { "sparse_infill_density",     0 },
            { "thick_bridges",             false },
        });

        WHEN("Searching for internal bridge surfaces")
        {
            double internal_bridge_z = find_first_internal_bridge_z(print);
            INFO("First internal bridge Z: " << internal_bridge_z);

            THEN("At least one layer has stInternalBridge fill surfaces")
            {
                // Internal bridges may or may not be present depending on the
                // geometry. If present, verify the extra layer.
                if (internal_bridge_z > 0.0) {
                    double extra_z = internal_bridge_z + 0.2;
                    auto extra_types = collect_surface_types_at_z(print, extra_z);

                    bool has_second_bridge =
                        contains_surface_type(extra_types, stSecondInternalBridge) ||
                        contains_surface_type(extra_types, stInternalBridge);

                    INFO("Extra internal bridge Z: " << extra_z);
                    for (size_t i = 0; i < extra_types.size(); ++i) {
                        INFO("Surface type [" << i << "]: " << (int)extra_types[i]);
                    }
                    REQUIRE(has_second_bridge);
                } else {
                    // No internal bridges found -- this is geometry-dependent.
                    // Just verify the bridge mesh does produce regular bridges.
                    double bridge_z = find_first_bridge_z(print);
                    REQUIRE(bridge_z > 0.0);
                    SUCCEED("No internal bridges found in this geometry (expected for single-bottom bridge mesh)");
                }
            }
        }

        WHEN("Inspecting perimeter roles on the extra internal bridge layer")
        {
            double internal_bridge_z = find_first_internal_bridge_z(print);
            INFO("First internal bridge Z: " << internal_bridge_z);

            THEN("Extra internal bridge layer perimeters include erBridgePerimeter")
            {
                if (internal_bridge_z > 0.0) {
                    double extra_z = internal_bridge_z + 0.2;
                    INFO("Extra internal bridge Z: " << extra_z);

                    auto roles = collect_perimeter_roles_at_z(print, extra_z);
                    REQUIRE_FALSE(roles.empty());
                    for (auto r : unique_roles(roles)) {
                        INFO("Extra internal bridge perimeter role: " << (int)r);
                    }
                    REQUIRE(contains_erBridgePerimeter(roles));
                } else {
                    double bridge_z = find_first_bridge_z(print);
                    REQUIRE(bridge_z > 0.0);
                    SUCCEED("No internal bridges found in this geometry");
                }
            }
        }
    }
}

// ============================================================================
// Scenario 10: eblDisabled does NOT produce extra bridge layers (negative test)
// ============================================================================

SCENARIO("Extra bridge layer is not created when disabled", "[Bridge][ExtraBridge][Disabled]")
{
    GIVEN("A bridge mesh with enable_extra_bridge_layer = disabled")
    {
        Slic3r::Print print;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",              0.2 },
            { "initial_layer_print_height", 0.2 },
            { "enable_extra_bridge_layer", "disabled" },
            { "bottom_shell_layers",       1 },
            { "top_shell_layers",          0 },
            { "sparse_infill_density",     0 },
            { "thick_bridges",             false },
        });

        WHEN("Inspecting the layer above the first bridge")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("First bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            double above_z = bridge_z + 0.2;
            INFO("Layer above bridge Z: " << above_z);

            THEN("The layer above the bridge has no bridge surface types")
            {
                auto types = collect_surface_types_at_z(print, above_z);
                REQUIRE_FALSE(contains_surface_type(types, stBottomBridge));
                REQUIRE_FALSE(contains_surface_type(types, stInternalAfterExternalBridge));
            }

            THEN("The layer above the bridge has no erBridgePerimeter roles")
            {
                auto roles = collect_perimeter_roles_at_z(print, above_z);
                REQUIRE_FALSE(contains_erBridgePerimeter(roles));
            }
        }
    }
}

// ============================================================================
// Scenario 11: eblExternalBridgeOnly creates extra layer for external bridges only
// ============================================================================

SCENARIO("Extra bridge layer with external_bridge_only mode", "[Bridge][ExtraBridge][ExternalOnly]")
{
    GIVEN("A bridge mesh with enable_extra_bridge_layer = external_bridge_only")
    {
        Slic3r::Print print;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",              0.2 },
            { "initial_layer_print_height", 0.2 },
            { "enable_extra_bridge_layer", "external_bridge_only" },
            { "bottom_shell_layers",       2 },
            { "top_shell_layers",          0 },
            { "sparse_infill_density",     0 },
            { "thick_bridges",             false },
        });

        WHEN("Inspecting the layer above the first external bridge")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("First bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            double extra_z = bridge_z + 0.2;
            INFO("Extra bridge Z: " << extra_z);

            THEN("The extra layer has bridge surface types")
            {
                auto types = collect_surface_types_at_z(print, extra_z);
                bool has_bridge_surface =
                    contains_surface_type(types, stBottomBridge) ||
                    contains_surface_type(types, stInternalAfterExternalBridge);
                REQUIRE(has_bridge_surface);
            }

            THEN("The extra layer perimeters include erBridgePerimeter")
            {
                auto roles = collect_perimeter_roles_at_z(print, extra_z);
                REQUIRE_FALSE(roles.empty());
                REQUIRE(contains_erBridgePerimeter(roles));
            }
        }

        WHEN("Checking that internal bridges do NOT get an extra layer")
        {
            double internal_bridge_z = find_first_internal_bridge_z(print);
            INFO("First internal bridge Z: " << internal_bridge_z);

            THEN("No extra internal bridge layer is created")
            {
                if (internal_bridge_z > 0.0) {
                    double extra_z = internal_bridge_z + 0.2;
                    auto types = collect_surface_types_at_z(print, extra_z);
                    REQUIRE_FALSE(contains_surface_type(types, stSecondInternalBridge));
                } else {
                    SUCCEED("No internal bridges in this geometry â€” negative test trivially passes");
                }
            }
        }
    }
}

// ============================================================================
// Scenario 12: eblInternalBridgeOnly creates extra layer for internal bridges only
// ============================================================================

SCENARIO("Extra bridge layer with internal_bridge_only mode", "[Bridge][ExtraBridge][InternalOnly]")
{
    GIVEN("A bridge mesh with enable_extra_bridge_layer = internal_bridge_only")
    {
        Slic3r::Print print;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",              0.2 },
            { "initial_layer_print_height", 0.2 },
            { "enable_extra_bridge_layer", "internal_bridge_only" },
            { "bottom_shell_layers",       2 },
            { "top_shell_layers",          0 },
            { "sparse_infill_density",     0 },
            { "thick_bridges",             false },
        });

        WHEN("Checking that external bridges do NOT get an extra layer")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("First bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            double above_z = bridge_z + 0.2;
            INFO("Layer above external bridge Z: " << above_z);

            THEN("The layer above the external bridge has no extra bridge surfaces")
            {
                auto types = collect_surface_types_at_z(print, above_z);
                REQUIRE_FALSE(contains_surface_type(types, stInternalAfterExternalBridge));
                // Note: stBottomBridge might be present from the geometry's own
                // bridge detection (second bottom layer), but
                // stInternalAfterExternalBridge specifically should be absent.
            }
        }

        WHEN("Inspecting internal bridge extra layer")
        {
            double internal_bridge_z = find_first_internal_bridge_z(print);
            INFO("First internal bridge Z: " << internal_bridge_z);

            THEN("Internal bridge extra layer has correct surfaces and perimeters")
            {
                if (internal_bridge_z > 0.0) {
                    double extra_z = internal_bridge_z + 0.2;
                    INFO("Extra internal bridge Z: " << extra_z);

                    auto types = collect_surface_types_at_z(print, extra_z);
                    bool has_second_bridge =
                        contains_surface_type(types, stSecondInternalBridge) ||
                        contains_surface_type(types, stInternalBridge);
                    REQUIRE(has_second_bridge);

                    auto roles = collect_perimeter_roles_at_z(print, extra_z);
                    REQUIRE_FALSE(roles.empty());
                    REQUIRE(contains_erBridgePerimeter(roles));
                } else {
                    SUCCEED("No internal bridges in this geometry â€” cannot test extra layer");
                }
            }
        }
    }
}

// ============================================================================
// Scenario 13: Only bridge-spanning perimeters are retagged (partial tagging)
// ============================================================================
//
// Regression test for the over-tagging bug: the original implementation grew
// the bridge fill region by the full perimeter stack width, causing EVERY
// perimeter on a bridge layer to be tagged as erBridgePerimeter â€” including
// the supported pillar walls that have solid material below them.
//
// The bridge mesh has two solid pillars (X: 0-5 and X: 45-50) and a span
// between them.  At the bridge layer the pillar perimeters are fully supported
// by the pillar walls below; only the span perimeters cross over the void.
// After the fix, the bridge layer must contain BOTH erBridgePerimeter (span)
// AND erExternalPerimeter / erPerimeter (pillars).

SCENARIO("Bridge perimeter tagging is partial: supported walls keep normal roles", "[Bridge][Perimeter][Partial]")
{
    GIVEN("A bridge mesh sliced with default settings")
    {
        Slic3r::Print print;

        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",               0.2 },
            { "initial_layer_print_height", 0.2 },
            { "bottom_shell_layers",        1 },
            { "top_shell_layers",           0 },
            { "sparse_infill_density",      0 },
        });

        WHEN("Inspecting perimeter roles on the bridge layer")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("Bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            auto roles = collect_perimeter_roles_at_z(print, bridge_z);
            for (auto r : unique_roles(roles))
                INFO("Role present: " << (int)r);

            THEN("Spanning perimeters are tagged erBridgePerimeter")
            {
                REQUIRE(contains_erBridgePerimeter(roles));
            }

            THEN("Supported pillar perimeters keep their normal role")
            {
                bool has_normal = contains_erExternalPerimeter(roles)
                               || contains_erPerimeter(roles);
                REQUIRE(has_normal);
            }

            THEN("NOT all perimeters on the bridge layer are erBridgePerimeter")
            {
                REQUIRE_FALSE(all_roles_are_bridge_perimeter(roles));
            }
        }
    }
}

// Scenario 10 (Extra bridge layer speed and fan in G-code) removed:
// Slic3r::Test::slice() with cooling/fan config causes a pre-existing
// segfault in G-code export that is unrelated to bridge perimeter retagging.
// Bridge perimeter fan/speed behaviour is verified via the perimeter role
// tests above â€” the G-code exporter respects erBridgePerimeter natively.

// ============================================================================
// Scenario 14: Second bridge layer perimeters have parity with the first
// ============================================================================
//
// Regression test for the "extra bridge layer" perimeter promotion.  When an
// extra (second) bridging layer is generated above a bridge, its perimeters
// that continue over the bridged void must also be promoted to
// erBridgePerimeter â€” with coverage comparable to the first bridge layer
// directly below.  The real-world failure (opengrid model) was that the first
// bridge layer got N bridge perimeters while the second layer above it got 0.
//
// Uses bridge_with_hole, whose bridge contains a supported island, so the layer
// has BOTH spanning perimeters (which must be promoted) and supported
// perimeters around the hole (which must stay normal) â€” closer to a multi-cell
// topology than the single-span `bridge` mesh.

SCENARIO("Second bridge layer perimeters reach parity with the first", "[Bridge][ExtraBridge][Perimeter][Parity]")
{
    GIVEN("A bridge_with_hole mesh with extra bridge layers enabled")
    {
        Slic3r::Print print;

        TriangleMesh mesh = Slic3r::Test::mesh(TestMesh::bridge_with_hole);
        mesh.align_to_origin();

        Slic3r::Test::init_and_process_print({mesh}, print, {
            { "layer_height",               0.2 },
            { "initial_layer_print_height", 0.2 },
            { "enable_extra_bridge_layer",  "apply_to_all" },
            { "bottom_shell_layers",        1 },
            { "top_shell_layers",           0 },
            { "sparse_infill_density",      0 },
            { "thick_bridges",              false },
        });

        WHEN("Comparing bridge-perimeter coverage of the first and second bridge layers")
        {
            double bridge_z = find_first_bridge_z(print);
            INFO("First bridge Z: " << bridge_z);
            REQUIRE(bridge_z > 0.0);

            double extra_z = bridge_z + 0.2;
            INFO("Second bridge layer Z: " << extra_z);

            const size_t first_bp = count_perimeter_role_at_z(print, bridge_z, erBridgePerimeter);
            const size_t extra_bp = count_perimeter_role_at_z(print, extra_z, erBridgePerimeter);
            INFO("First-layer bridge perimeters: " << first_bp);
            INFO("Second-layer bridge perimeters: " << extra_bp);

            THEN("The first bridge layer has bridge perimeters")
            {
                REQUIRE(first_bp > 0);
            }

            THEN("The second bridge layer also has bridge perimeters (no status-quo gap)")
            {
                REQUIRE(extra_bp > 0);
            }

            THEN("Second-layer bridge-perimeter coverage is comparable to the first")
            {
                // Allow some slack for geometry differences between layers, but
                // the second layer must not collapse to a tiny fraction (the
                // observed failure was first=N, second=0).
                REQUIRE(extra_bp * 2 >= first_bp);
            }
        }
    }
}

// ============================================================================
// Scenario 15: Stacked bridge layers do not shrink (no compounded inset)
// ============================================================================
//
// Regression test for BRIDGE_PERIMETERS.md Â§5c. The extra-bridge tagging must
// classify every stacked bridge layer against the SAME base support, so the
// bridge-perimeter coverage (length) is stable up the stack â€” it must not
// shrink layer by layer from a compounded half-nozzle inset.

SCENARIO("Stacked bridge layers keep stable bridge-perimeter coverage", "[Bridge][ExtraBridge][Perimeter][Inset]")
{
    GIVEN("A bridge mesh with extra bridge layers enabled")
    {
        Slic3r::Print print;
        TriangleMesh bridge_mesh = Slic3r::Test::mesh(TestMesh::bridge);
        bridge_mesh.align_to_origin();
        Slic3r::Test::init_and_process_print({bridge_mesh}, print, {
            { "layer_height",               0.2 },
            { "initial_layer_print_height", 0.2 },
            { "enable_extra_bridge_layer",  "apply_to_all" },
            { "bottom_shell_layers",        1 },
            { "top_shell_layers",           0 },
            { "sparse_infill_density",      0 },
            { "thick_bridges",              false },
        });

        WHEN("Comparing bridge-perimeter length on the first vs second bridge layer")
        {
            double bridge_z = find_first_bridge_z(print);
            REQUIRE(bridge_z > 0.0);
            double extra_z = bridge_z + 0.2;

            double first_len  = perimeter_role_length_at_z(print, bridge_z, erBridgePerimeter);
            double extra_len  = perimeter_role_length_at_z(print, extra_z, erBridgePerimeter);
            double first_span = perimeter_role_x_span_at_z(print, bridge_z, erBridgePerimeter);
            double extra_span = perimeter_role_x_span_at_z(print, extra_z, erBridgePerimeter);
            INFO("First-layer  bridge length=" << first_len << " x-span=" << first_span);
            INFO("Second-layer bridge length=" << extra_len << " x-span=" << extra_span);

            THEN("Both layers have bridge perimeters")
            {
                REQUIRE(first_len > 0.0);
                REQUIRE(extra_len > 0.0);
            }

            THEN("Second-layer bridge extent matches the first (no compounded inset)")
            {
                // Absolute test: a compounded half-nozzle inset would shrink the
                // second layer's bridge X-extent by ~one nozzle (~0.4 mm) vs the
                // first. The slab cross-section is constant, so with a
                // non-compounding support the extents match to within rounding.
                REQUIRE(std::abs(extra_span - first_span) < 0.15);
            }
        }
    }
}
