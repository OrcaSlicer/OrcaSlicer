#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;

// Helper: Create a single ModelObject with two volumes on different extruders.
// This simulates MMU color painting where different regions of one body are
// assigned to different filaments.
static void init_mmu_print(TriangleMesh &&mesh1, TriangleMesh &&mesh2,
                           Print &print, Model &model,
                           const DynamicPrintConfig &config_overrides)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "filament_diameter", "1.75,1.75" },
        { "line_width",        "0.45" },
    });
    config.apply(config_overrides);

    ModelObject *object = model.add_object();
    object->name = "mmu_test_object";

    ModelVolume *v1 = object->add_volume(std::move(mesh1));
    v1->name = "base_volume";
    v1->config.set("extruder", 1);

    ModelVolume *v2 = object->add_volume(std::move(mesh2));
    v2->name = "bridge_volume";
    v2->config.set("extruder", 2);

    object->add_instance();
    object->ensure_on_bed();

    print.is_BBL_printer() = false;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
}

// Count surfaces of a given type across all layers/regions.
static int count_surface_type(const PrintObject &obj, SurfaceType type)
{
    int count = 0;
    for (const Layer *layer : obj.layers())
        for (const LayerRegion *lr : layer->regions())
            for (const Surface &s : lr->fill_surfaces.surfaces)
                if (s.surface_type == type)
                    ++count;
    return count;
}

// ============================================================================
// Test 1: A painted region spanning a gap (true bridge) should be stBottomBridge
// ============================================================================
TEST_CASE("MMU cross-region bridge over void is stBottomBridge", "[MMUBridge]") {
    // Geometry:
    //   Two pillars (extruder 1): 20x20x6mm at y=0..20 and y=30..50
    //   10mm gap at y=20..30
    //   Slab (extruder 2): 20x50x1mm at z=6..7, spanning both pillars + gap
    TriangleMesh pillar1 = make_cube(20, 20, 6);
    TriangleMesh pillar2 = make_cube(20, 20, 6);
    pillar2.translate(0, 30, 0);
    pillar1.merge(pillar2);

    TriangleMesh slab = make_cube(20, 50, 1);
    slab.translate(0, 0, 6);

    Print print;
    Model model;
    DynamicPrintConfig overrides;
    overrides.set_deserialize_strict({
        { "layer_height",       "0.3" },
        { "first_layer_height", "0.3" },
        { "bottom_shell_layers", "1" },
        { "top_shell_layers",   "1" },
    });
    init_mmu_print(std::move(pillar1), std::move(slab), print, model, overrides);
    print.process();

    const PrintObject &obj = *print.objects().front();
    REQUIRE(obj.num_printing_regions() > 1);

    // Count layers that have stBottomBridge surfaces.
    // Bridges should appear in exactly one layer (the first slab layer over the gap).
    int bridge_layer_count = 0;
    for (const Layer *layer : obj.layers()) {
        bool layer_has_bridge = false;
        for (const LayerRegion *lr : layer->regions())
            for (const Surface &s : lr->fill_surfaces.surfaces)
                if (s.surface_type == stBottomBridge)
                    layer_has_bridge = true;
        if (layer_has_bridge)
            bridge_layer_count++;
    }
    REQUIRE(bridge_layer_count == 1);
}

// ============================================================================
// Test 2: A painted region fully supported by different material is NOT stBottomBridge
// ============================================================================
TEST_CASE("MMU region on solid support is not stBottomBridge", "[MMUBridge]") {
    // Geometry:
    //   Base (extruder 1): 20x20x2mm
    //   Small patch (extruder 2): 8x8x1mm sitting on top of base at z=2mm
    // Patch is fully supported by base below -> NOT a bridge.
    TriangleMesh base = make_cube(20, 20, 2);
    TriangleMesh patch = make_cube(8, 8, 1);
    patch.translate(6, 6, 2);

    Print print;
    Model model;
    DynamicPrintConfig overrides;
    overrides.set_deserialize_strict({
        { "layer_height",       "0.3" },
        { "first_layer_height", "0.3" },
    });
    init_mmu_print(std::move(base), std::move(patch), print, model, overrides);
    print.process();

    const PrintObject &obj = *print.objects().front();
    REQUIRE(obj.num_printing_regions() > 1);

    int bridge_count = count_surface_type(obj, stBottomBridge);
    // There should be no cross-region bridges (patch is fully supported)
    CHECK(bridge_count == 0);
}

// ============================================================================
// Test 3: stBottomBridge survives when bottom_shell_layers=0
// ============================================================================
TEST_CASE("MMU bridge preserved with bottom_shell_layers=0", "[MMUBridge]") {
    // Same bridge geometry as Test 1, but with bottom_shell_layers=0.
    // The fix in prepare_fill_surfaces should preserve stBottomBridge (not convert to stInternal).
    TriangleMesh pillar1 = make_cube(20, 20, 6);
    TriangleMesh pillar2 = make_cube(20, 20, 6);
    pillar2.translate(0, 30, 0);
    pillar1.merge(pillar2);

    TriangleMesh slab = make_cube(20, 50, 1);
    slab.translate(0, 0, 6);

    Print print;
    Model model;
    DynamicPrintConfig overrides;
    overrides.set_deserialize_strict({
        { "layer_height",        "0.3" },
        { "first_layer_height",  "0.3" },
        { "bottom_shell_layers", "0" },
        { "top_shell_layers",    "1" },
    });
    init_mmu_print(std::move(pillar1), std::move(slab), print, model, overrides);
    print.process();

    const PrintObject &obj = *print.objects().front();
    REQUIRE(obj.num_printing_regions() > 1);

    // stBottomBridge should survive prepare_fill_surfaces even with bottom_shell_layers=0.
    // Count layers with bridges - should be exactly one.
    int bridge_layer_count = 0;
    for (const Layer *layer : obj.layers()) {
        bool layer_has_bridge = false;
        for (const LayerRegion *lr : layer->regions())
            for (const Surface &s : lr->fill_surfaces.surfaces)
                if (s.surface_type == stBottomBridge)
                    layer_has_bridge = true;
        if (layer_has_bridge)
            bridge_layer_count++;
    }
    REQUIRE(bridge_layer_count == 1);
}

// ============================================================================
// Test 4: Bridge fill covers the gap
// ============================================================================
TEST_CASE("MMU bridge fill covers the gap area", "[MMUBridge]") {
    // Same geometry as Test 1. Verify stBottomBridge surfaces at the bridge layer
    // have their bounding box within the gap region (y~20..30) and area is
    // approximately the gap area (20x10 = 200mm2).
    TriangleMesh pillar1 = make_cube(20, 20, 6);
    TriangleMesh pillar2 = make_cube(20, 20, 6);
    pillar2.translate(0, 30, 0);
    pillar1.merge(pillar2);

    TriangleMesh slab = make_cube(20, 50, 1);
    slab.translate(0, 0, 6);

    Print print;
    Model model;
    DynamicPrintConfig overrides;
    overrides.set_deserialize_strict({
        { "layer_height",       "0.3" },
        { "first_layer_height", "0.3" },
        { "bottom_shell_layers", "1" },
        { "top_shell_layers",   "1" },
    });
    init_mmu_print(std::move(pillar1), std::move(slab), print, model, overrides);
    print.process();

    const PrintObject &obj = *print.objects().front();
    REQUIRE(obj.num_printing_regions() > 1);

    // Find the bridge layer.
    const Layer *bridge_layer = nullptr;
    for (const Layer *layer : obj.layers()) {
        if (bridge_layer)
            break;
        for (const LayerRegion *lr : layer->regions()) {
            if (bridge_layer)
                break;
            for (const Surface &s : lr->fill_surfaces.surfaces)
                if (s.surface_type == stBottomBridge) {
                    bridge_layer = layer;
                    break;
                }
        }
    }
    REQUIRE(bridge_layer != nullptr);

    // At the bridge layer, check bridge fill covers the gap.
    // Note: ensure_on_bed() centers the object. With y spanning 0..50, the center
    // is at y=25, so the gap at y=20..30 becomes y=-5..5 in print coordinates.
    bool found_gap_bridge = false;
    for (const LayerRegion *lr : bridge_layer->regions()) {
        for (const Surface &s : lr->fill_surfaces.surfaces) {
            if (s.surface_type != stBottomBridge)
                continue;
            BoundingBox bb = get_extents(s.expolygon);
            double area_mm2 = unscale<double>(unscale<double>(s.expolygon.area()));
            // Bridge should be in the gap region (y=-5..5 after centering)
            CHECK(unscale<double>(bb.min.y()) >= -6.0);
            CHECK(unscale<double>(bb.max.y()) <= 6.0);
            // Bridge area should be close to gap area (200mm2)
            CHECK(area_mm2 > 100.0);  // at least half the gap
            found_gap_bridge = true;
        }
    }
    REQUIRE(found_gap_bridge);
}

// ============================================================================
// Test 5: Normal same-color bridge still works (regression test)
// ============================================================================
TEST_CASE("Normal single-material bridge still works", "[MMUBridge]") {
    // Single material: two pillars + bridge slab, all same extruder.
    // Same geometry as MMU tests but everything in one volume/region.
    // Standard bridge detection should find stBottomBridge surfaces.
    TriangleMesh left_pillar  = make_cube(5, 20, 4);
    TriangleMesh right_pillar = make_cube(5, 20, 4);
    right_pillar.translate(15, 0, 0);
    left_pillar.merge(right_pillar);

    TriangleMesh bridge_slab = make_cube(20, 20, 0.6);
    bridge_slab.translate(0, 0, 4);
    left_pillar.merge(bridge_slab);

    Print print;
    Model model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",       "0.3" },
        { "first_layer_height", "0.3" },
        { "bottom_shell_layers", "1" },
        { "top_shell_layers",   "1" },
    });

    ModelObject *object = model.add_object();
    object->name = "single_material_bridge";
    object->add_volume(std::move(left_pillar));
    object->add_instance();
    object->ensure_on_bed();

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    const PrintObject &obj = *print.objects().front();

    bool found_bridge = false;
    for (const Layer *layer : obj.layers())
        for (const LayerRegion *lr : layer->regions())
            for (const Surface &s : lr->fill_surfaces.surfaces)
                if (s.surface_type == stBottomBridge)
                    found_bridge = true;

    CHECK(found_bridge);
}

// Helper: Create face-painted pillars+slab geometry and process through full MMU pipeline.
// Two pillars (20x20x6) at y=0..20 and y=30..50 with a 10mm gap, merged with a
// slab (20x50x1) at z=6..7. Only bottom face triangles of the slab are painted as
// extruder 2. Caller owns Print and Model (must outlive returned reference).
static const PrintObject &init_face_painted_bridge_print(Print &print, Model &model)
{
    TriangleMesh pillars = make_cube(20, 20, 6);
    TriangleMesh pillar2 = make_cube(20, 20, 6);
    pillar2.translate(0, 30, 0);
    pillars.merge(pillar2);

    TriangleMesh bridge_slab = make_cube(20, 50, 1);

    // Before translation, find bottom face triangles (all 3 vertices have z ≈ 0).
    std::vector<int> bottom_face_indices;
    for (int i = 0; i < (int)bridge_slab.its.indices.size(); ++i) {
        const auto &tri = bridge_slab.its.indices[i];
        bool all_bottom = true;
        for (int j = 0; j < 3; ++j)
            if (bridge_slab.its.vertices[tri[j]].z() > 0.01f)
                all_bottom = false;
        if (all_bottom)
            bottom_face_indices.push_back(i);
    }
    assert(bottom_face_indices.size() > 0);

    bridge_slab.translate(0, 0, 6);

    // Remember where pillar triangles end and slab triangles begin.
    int pillar_tri_count = pillars.facets_count();
    pillars.merge(bridge_slab);
    int total_tri_count = pillars.facets_count();

    // Face-painted tests need ALL config keys (PrintConfig + PrintObjectConfig + PrintRegionConfig)
    // because apply_mm_segmentation() accesses PrintConfig keys like nozzle_diameter,
    // outer_wall_line_width, and filament_colour (used as num_facets_states = filament_colour.size()+1).
    // full_print_config() only includes PrintRegionConfig keys.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply((const PrintConfig &)FullPrintConfig::defaults());
    config.apply((const PrintObjectConfig &)FullPrintConfig::defaults());
    config.set_deserialize_strict({
        { "filament_diameter",      "1.75,1.75" },
        { "filament_colour",        "#FFFFFF;#FFFFFF" },
        { "line_width",             "0.45" },
        { "outer_wall_line_width",  "0.45" },
        { "layer_height",           "0.3" },
        { "first_layer_height",     "0.3" },
        { "bottom_shell_layers",    "1" },
        { "top_shell_layers",       "1" },
    });

    ModelObject *object = model.add_object();
    object->name = "painted_bridge";

    ModelVolume *vol = object->add_volume(std::move(pillars));
    vol->config.set("extruder", 1);

    // Paint ONLY bottom face triangles of the slab as extruder 2.
    // Bitstream encoding: nibble = (state << 2) | split_sides.  For leaf state 2:
    // nibble = 0b1000 = 8 hex.  (NOT "2" - that encodes split_sides=2, not state=2.)
    // set_triangle_from_string requires strictly increasing triangle IDs.
    vol->mmu_segmentation_facets.reserve(total_tri_count);
    for (int idx : bottom_face_indices)
        vol->mmu_segmentation_facets.set_triangle_from_string(pillar_tri_count + idx, "8");
    vol->mmu_segmentation_facets.shrink_to_fit();

    object->add_instance();
    object->ensure_on_bed();

    print.is_BBL_printer() = false;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    return *print.objects().front();
}

// ============================================================================
// Test 6: Face-painted bridge through full MMU segmentation pipeline
// ============================================================================
TEST_CASE("MMU face-painted bridge full pipeline", "[MMUBridge]") {
    // Single ModelVolume with mmu_segmentation_facets - the actual face painting path.
    // Geometry: two pillars + slab merged into one mesh.
    // Only the BOTTOM FACE triangles of the slab are painted as extruder 2.
    // This is the realistic scenario: painting the hanging face a different color.
    // Without the fix, the face-painted bottom region gets overhanging walls
    // instead of proper bridges.
    Print print;
    Model model;
    const PrintObject &obj = init_face_painted_bridge_print(print, model);
    REQUIRE(obj.num_printing_regions() > 1);

    // The painted bottom-face region over the gap must get proper bridge fills,
    // NOT overhanging walls.  On main (without the fix), the region gets
    // erOverhangPerimeter extrusions instead of stBottomBridge fills.
    int bridge_layer_count = 0;
    bool found_overhang_perimeter = false;
    for (const Layer *layer : obj.layers()) {
        bool layer_has_bridge = false;
        for (const LayerRegion *lr : layer->regions()) {
            for (const Surface &s : lr->fill_surfaces.surfaces)
                if (s.surface_type == stBottomBridge)
                    layer_has_bridge = true;
            // Check for overhang perimeters in the bridge z-range.
            if (layer->print_z > 5.5 && layer->print_z < 7.0) {
                for (const ExtrusionEntity *ee : lr->perimeters.flatten().entities)
                    if (ee->role() == erOverhangPerimeter)
                        found_overhang_perimeter = true;
            }
        }
        if (layer_has_bridge)
            bridge_layer_count++;
    }
    REQUIRE(bridge_layer_count == 1);
    // With the fix, the gap region gets bridge fills - no overhang perimeters.
    // Without the fix (main), the painted region produces overhang walls instead.
    CHECK_FALSE(found_overhang_perimeter);
}

