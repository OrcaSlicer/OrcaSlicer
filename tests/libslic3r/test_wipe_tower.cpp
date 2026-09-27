#include <catch2/catch_all.hpp>

#include <cmath>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTower2.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// A Bambu P1S project that reproduced the off-plate brim: two PLAs priming 30 and 45 mm3 in
// separate adhesiveness categories on a 35 mm tower, 0.21 mm layers, 0.4 nozzle (0.5 mm lines),
// 150 % infill gap (0.75 mm line pitch), rib width 8, 16 mm tall.
static std::vector<WipeTower::PurgeEstimate> cube_purges(int first_category = 100)
{
    return {{30.f, first_category}, {45.f, 0}};
}

TEST_CASE("Cone base polygon bulges past the body box", "[WipeTower]") {
    // Zero angle: plain body box.
    const Polygon box = WipeTower2::cone_base_polygon(35., 20., 100., 0.);
    CHECK(box.points.size() == 4);
    CHECK(get_extents(box).size() == Point::new_scale(Vec2d(35., 20.)));
    // A 25-degree cone on a 100 mm tower: base radius R = tan(12.5deg)*100 = 22.2 mm,
    // which exceeds the body half-depth, so the footprint bulges to center +- R in y
    // (support_scale keeps the x extent compressed near the body).
    const Polygon     base = WipeTower2::cone_base_polygon(35., 20., 100., 25.);
    const BoundingBox bb   = get_extents(base);
    const double      R    = std::tan(25. / 2. * M_PI / 180.) * 100.;
    CHECK_THAT(unscaled(bb.min.y()), WithinAbs(10. - R, 0.1));
    CHECK_THAT(unscaled(bb.max.y()), WithinAbs(10. + R, 0.1));
    // The footprint always contains the body box.
    CHECK(diff(Polygons{box}, Polygons{base}).empty());
}

TEST_CASE("Type1 block-stack depth quantizes each purge to whole lines", "[WipeTower]") {
    // A 0.5 mm line at 0.21 mm carries 0.0955 mm3 per mm, so across the 34 mm between the
    // perimeters 30 mm3 is 10 lines and 45 mm3 is 14: 7.5 + 10.5 at the 0.75 mm pitch behind
    // one perimeter width. The generated mesh of the project measured exactly this.
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(cube_purges(), 35.f, 0.21f, 0.4f, 1.5f), WithinAbs(18.5f, 0.01f));
    // Sharing one category, a layer can never purge into every filament (one of them starts
    // the layer), so the block is sized by its worst layer and the 10-line purge drops out.
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(cube_purges(0), 35.f, 0.21f, 0.4f, 1.5f), WithinAbs(11.0f, 0.01f));
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth({}, 35.f, 0.2f, 0.4f, 1.f), WithinAbs(0.f, 1e-6f));
    // A width narrower than two perimeter widths cannot hold purge lines.
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth({{45.f, 0}}, 0.9f, 0.2f, 0.4f, 1.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("A nozzle change adds its ramming lines to the block", "[WipeTower]") {
    // 10 mm of 1.75 mm filament (24.05 mm3) laid as 1.0 mm nozzle-change lines at 0.2 mm
    // (0.1914 mm2 each) is 125.7 mm; across the 48.5 mm available that is 3 lines of 1.0 mm.
    std::vector<WipeTower::PurgeEstimate> purges{{100.f, 0}, {100.f, 0}};
    const float without_change = WipeTower::estimate_tower_blocks_depth(purges, 50.f, 0.2f, 0.4f, 1.f);
    purges.front().filament_change_length = 10.f;
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(purges, 50.f, 0.2f, 0.4f, 1.f) - without_change, WithinAbs(3.f, 1e-4f));
}

TEST_CASE("Rib tower footprint estimate covers the generated footprint", "[WipeTower]") {
    // The generated first-layer wall bbox of the project measured 29.56 mm from the sliced
    // G-code; the volume-only estimate said 23.585 mm.
    const float side = WipeTower::estimate_rib_tower_bbox_side(cube_purges(), 35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f);
    CHECK(side >= 29.56f);
    CHECK(side <= 29.56f + 4.f); // without grossly over-reserving plate space
    // Separate categories stack their blocks, so the footprint must not shrink when they differ.
    CHECK(side >= WipeTower::estimate_rib_tower_bbox_side(cube_purges(0), 35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f));
    CHECK_THAT(WipeTower::estimate_rib_tower_bbox_side({}, 35.f, 0.2f, 0.4f, 1.f, 8.f, 0.f, 16.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("Rib footprint extends the ribs, not the body, below the stability minimum", "[WipeTower]") {
    // A 10 mm body under a 90 mm print: the ribs stretch to the minimum depth's diagonal, and
    // the rib width is capped at half the body, so the square grows to minimum + 5 / sqrt(2).
    const float min_depth = WipeTower::get_limit_depth_by_height(90.f);
    REQUIRE(min_depth > 10.f);
    CHECK_THAT(WipeTower::rib_footprint_side(10.f, 10.f, 8.f, 0.f, 90.f), WithinAbs(min_depth + 5.f / std::sqrt(2.f), 1e-4f));
    // The extra rib length runs along the diagonal, so it shows as its projection on each axis.
    const float plain = WipeTower::rib_footprint_side(30.f, 30.f, 8.f, 0.f, 5.f);
    CHECK_THAT(plain, WithinAbs(30.f + 8.f / std::sqrt(2.f), 1e-4f));
    CHECK_THAT(WipeTower::rib_footprint_side(30.f, 30.f, 8.f, 4.f, 5.f) - plain, WithinAbs(4.f / std::sqrt(2.f), 1e-4f));
    // A negative extra length cannot pull the ribs inside the diagonal.
    CHECK_THAT(WipeTower::rib_footprint_side(30.f, 30.f, 8.f, -4.f, 5.f), WithinAbs(plain, 1e-4f));
    CHECK_THAT(WipeTower::rib_footprint_side(0.f, 30.f, 8.f, 0.f, 5.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("Brim width estimate matches each generator's loop quantization", "[WipeTower]") {
    // 3 mm configured, 0.4 nozzle, 0.2 first layer: 0.4571 mm spacing, 7 loops. WipeTower2
    // prints and reports the 7 loops; WipeTower reports half a spacing of line width on top.
    const float spacing = 0.5f - 0.2f * float(1. - M_PI_4);
    CHECK_THAT(WipeTower::estimate_brim_real_width(3.f, 0.4f, 0.2f, true), WithinAbs(7.f * spacing, 1e-4f));
    CHECK_THAT(WipeTower::estimate_brim_real_width(3.f, 0.4f, 0.2f, false), WithinAbs(7.5f * spacing, 1e-4f));
    CHECK_THAT(WipeTower::estimate_brim_real_width(0.f, 0.4f, 0.2f, true), WithinAbs(0.f, 1e-6f));
}

// ---------------------------------------------------------------------------------------------
// "No sparse layers": the compaction rule and the clearance it demands of the plate.
// ---------------------------------------------------------------------------------------------

// A square of side mm centred on (cx, cy), in bed coordinates.
static Polygon centered_square(double cx, double cy, double side)
{
    const double h = 0.5 * side;
    Polygon      poly;
    poly.points = {Point::new_scale(cx - h, cy - h), Point::new_scale(cx + h, cy - h),
                   Point::new_scale(cx + h, cy + h), Point::new_scale(cx - h, cy + h)};
    return poly;
}

static WipeTower::ToolChangeResult make_tcr(int initial_tool, int new_tool, float layer_height)
{
    WipeTower::ToolChangeResult tcr{};
    tcr.initial_tool = initial_tool;
    tcr.new_tool     = new_tool;
    tcr.layer_height = layer_height;
    return tcr;
}

// A 20 mm square tower at the bed origin, no spiral z-hop, so the keep-out zone is the bare
// footprint and every distance below is one the test sets.
static PrintConfig clearance_config()
{
    PrintConfig cfg;
    cfg.extruder_clearance_radius.value       = 40.;
    cfg.extruder_clearance_dist_to_rod.value  = 20.;
    cfg.extruder_clearance_height_to_rod.value = 25.;
    cfg.extruder_clearance_height_to_lid.value = 120.;
    cfg.nozzle_height.value                   = 5.;
    cfg.nozzle_diameter.values                = {0.4};
    cfg.z_hop.values                          = {0.};
    cfg.travel_slope.values                   = {3.};
    return cfg;
}

TEST_CASE("Sparse layers are skipped only when nothing else needs a tower on every layer", "[WipeTower][NoSparseLayers]") {
    PrintConfig cfg;
    cfg.timelapse_type.value            = TimelapseType::tlTraditional;
    cfg.enable_wrapping_detection.value = false;

    cfg.wipe_tower_no_sparse_layers.value = false;
    CHECK_FALSE(wipe_tower_sparse_layers_skipped(cfg));
    cfg.wipe_tower_no_sparse_layers.value = true;
    CHECK(wipe_tower_sparse_layers_skipped(cfg));

    // Both park the nozzle on the tower every layer, so no layer is ever dropped and the option
    // must read as off everywhere rather than compact in one place and not another.
    cfg.timelapse_type.value = TimelapseType::tlSmooth;
    CHECK_FALSE(wipe_tower_sparse_layers_skipped(cfg));
    cfg.timelapse_type.value            = TimelapseType::tlTraditional;
    cfg.enable_wrapping_detection.value = true;
    CHECK_FALSE(wipe_tower_sparse_layers_skipped(cfg));
}

TEST_CASE("A planned layer is sparse only when its single tool change keeps the filament", "[WipeTower][NoSparseLayers]") {
    CHECK(wipe_tower_layer_is_sparse({make_tcr(1, 1, 0.2f)}));
    CHECK_FALSE(wipe_tower_layer_is_sparse({make_tcr(0, 1, 0.2f)}));
    // A second entry means the layer carries real work whatever the tools are.
    CHECK_FALSE(wipe_tower_layer_is_sparse({make_tcr(1, 1, 0.2f), make_tcr(1, 1, 0.2f)}));
    CHECK_FALSE(wipe_tower_layer_is_sparse({}));
}

TEST_CASE("The compacted tower falls one layer height behind the object per sparse layer", "[WipeTower][NoSparseLayers]") {
    // Five 0.2 mm layers off a 0.1 mm z offset, the middle two sparse. The object reaches
    // 0.1 + 5 * 0.2 = 1.1; the tower only grows on the three printed layers, so it ends at
    // 0.1 + 3 * 0.2 = 0.7 and a sparse layer carries the previous value rather than its own.
    const std::vector<std::vector<WipeTower::ToolChangeResult>> tool_changes{
        {make_tcr(0, 1, 0.2f)}, {make_tcr(1, 1, 0.2f)}, {make_tcr(1, 1, 0.2f)},
        {make_tcr(1, 0, 0.2f)}, {make_tcr(0, 1, 0.2f)}};

    const std::vector<float> tower_z = compute_compacted_wipe_tower_z(tool_changes, 0.1f);
    REQUIRE(tower_z.size() == tool_changes.size());
    CHECK_THAT(tower_z[0], WithinAbs(0.3f, 1e-5f));
    CHECK_THAT(tower_z[1], WithinAbs(0.3f, 1e-5f));
    CHECK_THAT(tower_z[2], WithinAbs(0.3f, 1e-5f));
    CHECK_THAT(tower_z[3], WithinAbs(0.5f, 1e-5f));
    CHECK_THAT(tower_z[4], WithinAbs(0.7f, 1e-5f));
    CHECK_THAT(1.1f - tower_z.back(), WithinAbs(2 * 0.2f, 1e-5f));

    // Without a base the tower starts at the bed, and an empty layer carries over like a sparse one.
    const std::vector<float> no_offset = compute_compacted_wipe_tower_z({{make_tcr(0, 1, 0.2f)}, {}}, 0.f);
    CHECK_THAT(no_offset[0], WithinAbs(0.2f, 1e-5f));
    CHECK_THAT(no_offset[1], WithinAbs(0.2f, 1e-5f));
}

TEST_CASE("The tower keep-out zone grows by the spiral z-hop envelope", "[WipeTower][NoSparseLayers]") {
    PrintConfig   cfg       = clearance_config();
    const Polygon footprint = centered_square(0., 0., 20.);

    // No lift, no envelope: the zone works on the bare footprint.
    CHECK_THAT(unscaled(compacted_wipe_tower_zone(cfg, footprint).hull.bounding_box().max.x()), WithinAbs(10., 1e-6));

    // A spiral lift leaves the outline at low z, so it counts as tower. The circle reaches
    // 2 * lift / (2*pi*atan(slope)) past the outline, matching GCodeWriter: 2*2/(2*pi*atan(3)) = 0.51 mm.
    cfg.z_hop.values = {2.};
    const CompactedTowerZone lifted = compacted_wipe_tower_zone(cfg, footprint);
    CHECK_THAT(unscaled(lifted.hull.bounding_box().max.x()), WithinAbs(10.51, 0.02));
    CHECK_THAT(unscaled(lifted.hull.bounding_box().min.y()), WithinAbs(-10.51, 0.02));
    CHECK(diff(Polygons{footprint}, Polygons{lifted.hull}).empty());

    // z_hop is capped at 5 mm by the option, so a taller lift cannot widen the zone further.
    cfg.z_hop.values = {10.};
    const double capped = unscaled(compacted_wipe_tower_zone(cfg, footprint).hull.bounding_box().max.x());
    CHECK_THAT(capped, WithinAbs(10. + 2. * 5. / (2. * M_PI * std::atan(3.)), 0.02));

    // The rod sweeps the whole X axis, so its band is the tower's y span plus half the rod offset.
    CHECK_THAT(unscaled(lifted.bbox_rod.max.y()), WithinAbs(10.51 + 10., 0.02));
}

TEST_CASE("An object beside a compacted tower is limited by the nearest part of the toolhead", "[WipeTower][NoSparseLayers]") {
    const PrintConfig        cfg  = clearance_config();
    const CompactedTowerZone zone = compacted_wipe_tower_zone(cfg, centered_square(0., 0., 20.));

    // Each side carries half its clearance less 0.1 mm slack, so the two outlines meet when the
    // objects are a full clearance apart: 2 * (4 - 0.2) / 2 = 3.8 mm for the bare nozzle cone,
    // 2 * (40 - 0.2) / 2 = 39.8 mm for the head body. A 10 mm object at x leaves a gap of x - 15.
    const double tall = 50., shortish = 3.;

    // Gap 1 mm, inside the nozzle cone: the object may not rise above the tower at all.
    const CompactedTowerClearance touching = compacted_wipe_tower_clearance(cfg, zone, centered_square(16., 0., 10.), tall);
    CHECK_THAT(touching.allowed_rise, WithinAbs(0., 1e-9));

    // Gap 10 mm: clear of the cone but inside the head body, which starts at nozzle_height.
    const CompactedTowerClearance near_body = compacted_wipe_tower_clearance(cfg, zone, centered_square(25., 0., 10.), tall);
    CHECK(near_body.near_body);
    CHECK_THAT(near_body.allowed_rise, WithinAbs(5., 1e-9));
    CHECK_THAT(near_body.body_clearance, WithinAbs(40., 1e-9));

    // The same spot, but an object that never rises past the cone. The body sits above the cone, so
    // it cannot reach this object however close it stands, and only the narrow tier applies.
    const CompactedTowerClearance low = compacted_wipe_tower_clearance(cfg, zone, centered_square(25., 0., 10.), shortish);
    CHECK_FALSE(low.near_body);
    CHECK_THAT(low.body_clearance, WithinAbs(4., 1e-9));
    CHECK_THAT(low.allowed_rise, WithinAbs(25., 1e-9));

    // Gap 55 mm, clear of the head entirely: the rod is the obstacle, since the object shares the
    // tower's y band and the rod spans the whole x axis however far apart the two stand.
    const CompactedTowerClearance far_in_band = compacted_wipe_tower_clearance(cfg, zone, centered_square(70., 0., 10.), tall);
    CHECK_FALSE(far_in_band.near_body);
    CHECK_THAT(far_in_band.far_clearance, WithinAbs(25., 1e-9));
    CHECK_THAT(far_in_band.allowed_rise, WithinAbs(25., 1e-9));

    // Out of the band the rod passes over it and only the lid is left.
    const CompactedTowerClearance out_of_band = compacted_wipe_tower_clearance(cfg, zone, centered_square(70., 60., 10.), tall);
    CHECK_THAT(out_of_band.allowed_rise, WithinAbs(120., 1e-9));
}

TEST_CASE("The ring drawn around the tower meets the outline drawn around an offender", "[WipeTower][NoSparseLayers]") {
    const PrintConfig        cfg  = clearance_config();
    const CompactedTowerZone zone = compacted_wipe_tower_zone(cfg, centered_square(0., 0., 20.));

    // What the plater draws has to be what the check tested, otherwise a user moves an object until
    // the outlines part and slicing still refuses the plate. Both halves of the 3.8 mm nozzle
    // clearance: at a 3 mm gap the rings overlap and the rise limit is zero, at 5 mm neither holds.
    for (const auto &c : {std::make_pair(18., true), std::make_pair(20., false)}) {
        DYNAMIC_SECTION("object at x = " << c.first) {
            const Polygon                 hull      = centered_square(c.first, 0., 10.);
            const CompactedTowerClearance clearance = compacted_wipe_tower_clearance(cfg, zone, hull, 3.);
            const Polygons                rings     = compacted_wipe_tower_rings(zone, compacted_tower_body_tier(clearance));
            const Polygon                 outline   = compacted_wipe_tower_offender_outline(hull, clearance.body_clearance);
            const bool                    outlines_meet = ! intersection(rings, Polygons{outline}).empty();
            const bool                    rise_denied   = clearance.allowed_rise < EPSILON;
            CHECK(outlines_meet == c.second);
            CHECK(rise_denied == c.second);
        }
    }
}

TEST_CASE("Only the keep-out ring an object is measured against is drawn", "[WipeTower][NoSparseLayers]") {
    const PrintConfig        cfg  = clearance_config();
    const CompactedTowerZone zone = compacted_wipe_tower_zone(cfg, centered_square(0., 0., 20.));

    // Drawing the wide ring when no object is judged on it would show a keep-out zone the check can
    // never trip, so it is added only once some object reaches past the nozzle cone.
    CHECK(compacted_wipe_tower_rings(zone, false).size() == zone.grown_nozzle.size());
    CHECK(compacted_wipe_tower_rings(zone, true).size() == zone.grown_nozzle.size() + zone.grown_body.size());
    CHECK_THAT(unscaled(get_extents(zone.grown_nozzle).max.x()), WithinAbs(10. + 0.5 * (4. - 0.2), 0.02));
    CHECK_THAT(unscaled(get_extents(zone.grown_body).max.x()), WithinAbs(10. + 0.5 * (40. - 0.2), 0.02));
}

// ---------------------------------------------------------------------------------------------
// "Combine sparse layers": folding a run of toolchange-free layers into one thicker tower layer.
// ---------------------------------------------------------------------------------------------

TEST_CASE("Sparse layers are combined only when every layer is still the tower's to place", "[WipeTower][CombineSparseLayers]") {
    PrintConfig cfg;
    cfg.timelapse_type.value                     = TimelapseType::tlTraditional;
    cfg.enable_wrapping_detection.value          = false;
    cfg.wipe_tower_no_sparse_layers.value        = false;

    cfg.wipe_tower_sparse_layers_combination.value = false;
    CHECK_FALSE(wipe_tower_sparse_layers_combined(cfg));
    cfg.wipe_tower_sparse_layers_combination.value = true;
    CHECK(wipe_tower_sparse_layers_combined(cfg));

    // Dropping the sparse layers outright leaves nothing to combine.
    cfg.wipe_tower_no_sparse_layers.value = true;
    CHECK_FALSE(wipe_tower_sparse_layers_combined(cfg));
    CHECK(wipe_tower_sparse_layers_skipped(cfg));
    cfg.wipe_tower_no_sparse_layers.value = false;

    // Both of these park the nozzle on the tower every layer, so no layer may be folded away.
    cfg.timelapse_type.value = TimelapseType::tlSmooth;
    CHECK_FALSE(wipe_tower_sparse_layers_combined(cfg));
    cfg.timelapse_type.value            = TimelapseType::tlTraditional;
    cfg.enable_wrapping_detection.value = true;
    CHECK_FALSE(wipe_tower_sparse_layers_combined(cfg));
}

TEST_CASE("A layer folded into a later one is marked on the results the emitter reads", "[WipeTower][CombineSparseLayers]") {
    WipeTower::ToolChangeResult folded = make_tcr(1, 1, 0.2f);
    folded.combined_away               = true;
    CHECK(wipe_tower_layer_is_combined_away({folded}));
    CHECK_FALSE(wipe_tower_layer_is_combined_away({make_tcr(1, 1, 0.2f)}));
    CHECK_FALSE(wipe_tower_layer_is_combined_away({}));
}

TEST_CASE("A run of sparse layers prints once, on its last layer, at the height it covers", "[WipeTower][CombineSparseLayers]") {
    // Eight 0.1 mm layers on a 0.3 mm cap: a toolchange on the first and the last, sparse between.
    std::vector<float>      heights(8, 0.1f);
    const std::vector<char> sparse{0, 1, 1, 1, 1, 1, 1, 0};
    const std::vector<float> caps(8, 0.3f);

    const std::vector<char> combined = combine_sparse_wipe_tower_layers(heights, sparse, caps, 0);
    REQUIRE(combined.size() == heights.size());
    // Three layers fill the cap exactly: the run flushes on layers 3 and 6, the two below each go.
    CHECK(combined == std::vector<char>{0, 1, 1, 0, 1, 1, 0, 0});
    CHECK_THAT(heights[3], WithinAbs(0.3f, 1e-5f));
    CHECK_THAT(heights[6], WithinAbs(0.3f, 1e-5f));
    // Layers that print keep the object covered: nothing is lost and nothing is printed twice.
    float printed = 0.f;
    for (size_t i = 0; i < heights.size(); ++i)
        if (! combined[i])
            printed += heights[i];
    CHECK_THAT(printed, WithinAbs(0.8f, 1e-5f));
    // A toolchange has to purge at its own z, so those layers are left exactly as planned.
    CHECK_THAT(heights[0], WithinAbs(0.1f, 1e-5f));
    CHECK_THAT(heights[7], WithinAbs(0.1f, 1e-5f));
}

TEST_CASE("The maximum layer height of the nozzle that prints the run caps the merge", "[WipeTower][CombineSparseLayers]") {
    // The cap that counts belongs to the layer that prints the run; one that prints nothing lays
    // nothing down, so its own cap cannot constrain it. Five 0.1 mm layers, sparse above the first,
    // layer 3's nozzle taking only 0.15. (A real run holds one filament, so this only tests the
    // look-ahead.)
    std::vector<float>      heights(5, 0.1f);
    std::vector<float>      caps(5, 0.3f);
    caps[3]                          = 0.15f;
    const std::vector<char> combined = combine_sparse_wipe_tower_layers(heights, {0, 1, 1, 1, 1}, caps, 0);
    // Layer 2 cannot hand its 0.2 mm on to layer 3, so it prints there and a fresh run starts above.
    CHECK(combined == std::vector<char>{0, 1, 0, 1, 0});
    CHECK_THAT(heights[2], WithinAbs(0.2f, 1e-5f));
    CHECK_THAT(heights[4], WithinAbs(0.2f, 1e-5f));

    // A single layer already past the cap is printed as planned rather than shrunk.
    std::vector<float> tall{0.2f, 0.4f, 0.4f};
    const std::vector<char> tall_combined = combine_sparse_wipe_tower_layers(tall, {0, 1, 1}, {0.3f, 0.3f, 0.3f}, 0);
    CHECK(tall_combined == std::vector<char>{0, 0, 0});
    CHECK_THAT(tall[1], WithinAbs(0.4f, 1e-5f));
}

TEST_CASE("The tower's first layer is never folded away", "[WipeTower][CombineSparseLayers]") {
    // It carries the brim and has to sit on the bed, however little it purges.
    std::vector<float>      heights(4, 0.1f);
    const std::vector<char> combined = combine_sparse_wipe_tower_layers(heights, {1, 1, 1, 1}, std::vector<float>(4, 0.5f), 0);
    CHECK(combined.front() == 0);
    CHECK_THAT(heights.front(), WithinAbs(0.1f, 1e-5f));
    // Everything above it merges into the top layer, which the cap still fits.
    CHECK(combined == std::vector<char>{0, 1, 1, 0});
    CHECK_THAT(heights.back(), WithinAbs(0.3f, 1e-5f));
}

TEST_CASE("Footprint padding covers the brim and the extrusion half width on each side", "[WipeTower][NoSparseLayers]") {
    // A nominal outline hulls extrusion centre lines and is re-centred once the real wall is known,
    // so a line width per side on top of the brim is what keeps an estimate enclosing the real tower.
    const PrintConfig cfg = clearance_config();
    CHECK_THAT(compacted_tower_footprint_padding(cfg, 2.), WithinAbs(2. + 2. * 0.4, 1e-9));
    CHECK_THAT(compacted_tower_footprint_padding(cfg, 0.), WithinAbs(2. * 0.4, 1e-9));
    // Callers whose outline already carries the brim pass zero, and a negative one cannot shrink it.
    CHECK_THAT(compacted_tower_footprint_padding(cfg, -5.), WithinAbs(2. * 0.4, 1e-9));
}
