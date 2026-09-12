#include <catch2/catch_all.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Taken from the config enum map rather than hand-listed, so a flavor added to GCodeFlavor later
// is covered here without editing this file.
static std::vector<GCodeFlavor> non_klipper_flavors()
{
    std::vector<GCodeFlavor> flavors;
    for (const auto &[name, value] : ConfigOptionEnum<GCodeFlavor>::get_enum_values())
        if (GCodeFlavor(value) != gcfKlipper)
            flavors.push_back(GCodeFlavor(value));
    return flavors;
}

static std::string flavor_name(GCodeFlavor flavor)
{
    return ConfigOptionEnum<GCodeFlavor>::get_enum_names()[int(flavor)];
}

TEST_CASE("Klipper flushes the wipe tower planner queue with M400", "[WipeTower]")
{
    CHECK(std::string(flush_planner_queue_command(gcfKlipper)) == "M400\n");
}

TEST_CASE("Other flavors flush the wipe tower planner queue with a zero dwell", "[WipeTower]")
{
    const GCodeFlavor flavor = GENERATE(from_range(non_klipper_flavors()));
    INFO("gcode flavor: " << flavor_name(flavor));
    CHECK(std::string(flush_planner_queue_command(flavor)) == "G4 S0\n");
}

// 1.5s is exactly representable as a float, so neither form can drift when rounded.
TEST_CASE("Klipper waits in the wipe tower with a millisecond dwell", "[WipeTower]")
{
    CHECK(wait_command(gcfKlipper, 1.5f) == "G4 P1500\n");
}

TEST_CASE("Other flavors wait in the wipe tower with a seconds dwell", "[WipeTower]")
{
    const GCodeFlavor flavor = GENERATE(from_range(non_klipper_flavors()));
    INFO("gcode flavor: " << flavor_name(flavor));
    CHECK(wait_command(flavor, 1.5f) == "G4 S1.500\n");
}

// The prime tower is validated against the real printable outline, so the placement clamps have to
// agree with it wherever that outline is not a rectangle. A regular hexagon inscribed in a 200mm
// circle stands in for the shipped delta beds.
TEST_CASE("The wipe tower placement clamp follows a non-rectangular bed outline", "[WipeTower]")
{
    const coord_t margin = scaled<coord_t>(1.);
    auto square_at = [](double x, double y, double side) {
        return BoundingBox(Point::new_scale(x, y), Point::new_scale(x + side, y + side));
    };
    // Does the footprint, padded by pad, sit inside the outline once the returned move is applied?
    auto lands_inside = [](BoundingBox box, const Polygons &bed, const Vec2f &move, coord_t pad) {
        box.translate(Point::new_scale(move.x(), move.y()));
        return diff(Polygons{box.inflated(pad).polygon()}, bed).empty();
    };

    const Polygons hex_bed{make_circle_num_segments(scaled<double>(100.), 6)};
    const Polygons square_bed{Polygon::new_scale(Pointfs{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}})};

    SECTION("a rectangular bed is left to the bounding box clamp") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(50., 50., 30.), square_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(0., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }

    // Dragging the tower off one edge may not pull it away from the other, or it would jump out from
    // under the cursor instead of sliding along the edge.
    SECTION("only the violated axis is clamped") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(185., 50., 30.), square_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(-16., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }

    SECTION("a footprint already inside the outline is left alone") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(-15., -15., 30.), hex_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(0., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }

    SECTION("a footprint in the bounding box corner is pulled onto the bed") {
        const BoundingBox box = square_at(55., 50., 30.);
        REQUIRE_FALSE(lands_inside(box, hex_bed, Vec2f::Zero(), margin)); // in the bbox, off the hexagon
        CHECK(lands_inside(box, hex_bed, WipeTower::move_box_inside_polygon(box, hex_bed, margin), margin));
    }

    // An unresolved auto brim width reaches the drag clamp as a negative margin. Padding by it would
    // shrink the footprint and hand back a position the slice validation still rejects.
    SECTION("a negative margin still lands the footprint inside the outline") {
        const BoundingBox box = square_at(55., 50., 30.);
        const coord_t     brim = scaled<coord_t>(-0.5);
        CHECK(lands_inside(box, hex_bed, WipeTower::move_box_inside_polygon(box, hex_bed, brim), 0));
    }

    SECTION("a footprint too large for the bed is left alone") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(-200., -200., 400.), hex_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(0., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }
}

// The cases above only exercise the helpers in isolation. The one below slices a real
// two-filament print, so it also covers the binding constraint of both changes: that the
// configured `gcode_flavor` reaches the wipe tower writer and lands in the exported G-code.

// The G-code inside each WIPE_TOWER_START/WIPE_TOWER_END pair, concatenated, so an M400 emitted
// outside the tower (e.g. GCodeProcessor's pre-heat injector) cannot create a false match.
static std::string wipe_tower_regions(const std::string &gcode)
{
    const std::string &start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start);
    const std::string &end_tag   = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End);
    std::string regions;
    size_t pos = 0;
    while (true) {
        size_t start = gcode.find(start_tag, pos);
        if (start == std::string::npos)
            break;
        size_t end = gcode.find(end_tag, start);
        if (end == std::string::npos)
            break;
        regions.append(gcode, start, end - start);
        pos = end + 1;
    }
    return regions;
}

// A per-layer toolchange between the wall and infill filaments, same shape as
// test_multifilament.cpp's "Each feature prints with its assigned filament", so the wipe tower
// runs its toolchange path (and so `flush_planner_queue()`) on every layer.
static DynamicPrintConfig wipe_tower_toolchange_config(const std::string &gcode_flavor)
{
    return multifilament_config(2, {
        { "sparse_infill_filament_id",  1 },
        { "internal_solid_filament_id", 1 },
        { "top_surface_filament_id",    1 },
        { "bottom_surface_filament_id", 1 },
        { "outer_wall_filament_id",     2 },
        { "inner_wall_filament_id",     2 },
        { "enable_prime_tower",         true },
        { "wipe_tower_x",               50 }, // inside the 200x200 test bed
        { "wipe_tower_y",               50 }, // (the default y, 220, is not)
        { "layer_height",               0.3 },
        { "gcode_flavor",               gcode_flavor },
    });
}

// Slices a 10mm cube under `config`. Not plain Test::slice: a brand-new Print's first `apply()`
// counts one filament in use, and DynamicPrintConfig::normalize_fdm_2's single-filament rule then
// clears `enable_prime_tower`. A second apply, once init_print's regions have settled, sees both
// filaments and the tower survives.
static std::string slice_with_prime_tower(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    init_print({ cube(10) }, print, model, config);
    print.apply(model, config);
    return gcode(print);
}

TEST_CASE("The wipe tower's toolchange planner flush follows the gcode flavor", "[WipeTower]")
{
    auto [flavor, expected, unexpected] = GENERATE(table<std::string, std::string, std::string>({
        { "klipper", "M400",  "G4 S0" },
        { "marlin",  "G4 S0", "M400"  } }));
    DYNAMIC_SECTION(flavor) {
        const std::string tower = wipe_tower_regions(slice_with_prime_tower(wipe_tower_toolchange_config(flavor)));
        REQUIRE_FALSE(tower.empty());
        CHECK_THAT(tower, Catch::Matchers::ContainsSubstring(expected));
        CHECK_THAT(tower, !Catch::Matchers::ContainsSubstring(unexpected));
    }
}

// What Print feeds the shared estimate. The libslic3r WipeTowerEstimate cases cannot see this:
// they call the estimator directly. The estimate counts the filaments the print really uses,
// so the two-filament shape gives the outer wall the second one.
static DynamicPrintConfig tower_estimate_config(const char *wall_type, unsigned int filaments = 2)
{
    // 100 mm3 per purge on a 50 mm wide tower: one purge is 100/(layer_height * 50) of depth.
    return multifilament_config(filaments, {
        { "outer_wall_filament_id",         filaments == 2 ? "2" : "1" },
        { "enable_prime_tower",             "1"       },
        { "wipe_tower_wall_type",           wall_type },
        { "prime_tower_width",              "50"      },
        { "prime_volume",                   "100"     },
        { "prime_tower_infill_gap",         "100%"    },
        { "prime_tower_brim_width",         "3"       },
        { "purge_in_prime_tower",           "0"       },
        { "single_extruder_multi_material", "0"       },
        { "timelapse_type",                 "0"       },
        { "layer_height",                   "0.2"     },
        { "enable_wrapping_detection",      "0"       },
        { "raft_layers",                    "0"       } });
}

TEST_CASE("The tower is sized for the thinnest layer any object on the plate is sliced at", "[WipeTower]")
{
    // The tower has to survive its thinnest layer, so an override finer than the preset drives
    // the estimate even on the second object. Two 20 mm cubes, the second at 0.1 mm.
    const DynamicPrintConfig config = tower_estimate_config("rectangle");
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
        {}, { { "layer_height", "0.1" } } };

    Print print;
    Model model;
    init_print({ cube(20), cube(20) }, print, model, config, &overrides);

    // One purge at 0.1 mm: 100 / (0.1 * 50) = 20 mm, above the 20 mm-tall tower's stability
    // floor. At the preset's 0.2 mm it would be half that, so the two are easy to tell apart.
    const float floor_20mm = WipeTower::get_limit_depth_by_height(20.f);
    REQUIRE(floor_20mm < 10.f);
    CHECK_THAT(print.wipe_tower_data(2).depth, Catch::Matchers::WithinAbs(20., 1e-4));
}

TEST_CASE("Validation is given the tower's effective width, not the configured one", "[WipeTower]")
{
    // A rib wall squares the tower, so its width is its depth. Validation reads this rather
    // than re-deriving the rule from the wall type.
    Print print;
    Model model;

    SECTION("a rectangle wall keeps the configured width") {
        const DynamicPrintConfig config = tower_estimate_config("rectangle");
        init_print({ cube(20) }, print, model, config);
        const WipeTowerData &data = print.wipe_tower_data(2);
        CHECK_THAT(data.width, Catch::Matchers::WithinAbs(50., 1e-4));
        CHECK(data.depth < data.width);
    }

    SECTION("a rib wall reports the squared footprint") {
        const DynamicPrintConfig config = tower_estimate_config("rib");
        init_print({ cube(20) }, print, model, config);
        const WipeTowerData &data = print.wipe_tower_data(2);
        CHECK_THAT(data.width, Catch::Matchers::WithinAbs(data.depth, 1e-4));
        CHECK(data.width > 0.f);
    }
}

TEST_CASE("Generating the tower keeps its reported width current", "[WipeTower]")
{
    // width is handed out after the slice, so leaving it at the estimate reports a zero-width
    // tower to every post-generation consumer.
    const DynamicPrintConfig config = wipe_tower_toolchange_config("marlin");
    Print print;
    Model model;
    init_print({ cube(10) }, print, model, config);
    print.apply(model, config);
    REQUIRE(print.wipe_tower_data(2).width > 0.f);

    print.process();
    REQUIRE(print.is_step_done(psWipeTower));
    const WipeTowerData &data = print.wipe_tower_data();
    // A width the generator never wrote reads as zero. A rib wall squares the tower, so the
    // generated width is the body square: under the configured 50 mm, and inside the depth.
    CHECK(data.width > 0.f);
    CHECK(data.width < 50.f);
    CHECK(data.width <= data.depth + EPSILON);
}

TEST_CASE("A single-filament plate reserves a tower only when one is actually printed", "[WipeTower]")
{
    // The estimate has to answer this the way Print::apply does: reporting no tower for one
    // that is built collapses the validation hull to a point, and reporting one for a tower
    // that is not built takes that bed area away from the arranger and draws a preview box
    // over nothing.
    Print print;
    Model model;

    SECTION("no tool change and nothing else that prints one") {
        const DynamicPrintConfig config = tower_estimate_config("rib", 1);
        init_print({ cube(20) }, print, model, config);
        REQUIRE_FALSE(print.has_wipe_tower());
        CHECK_THAT(print.wipe_tower_data(1).depth, Catch::Matchers::WithinAbs(0., 1e-6));
    }

    // A raft puts the tower on every layer below the object, but only where there is a tower:
    // Print::apply runs normalize_fdm_2, which clears enable_prime_tower for a plate that
    // purges one filament and has neither smooth timelapse nor wrapping detection on.
    SECTION("a raft alone does not print one") {
        DynamicPrintConfig config = tower_estimate_config("rib", 1);
        config.set_deserialize_strict({ { "raft_layers", "3" } });
        init_print({ cube(20) }, print, model, config);
        REQUIRE_FALSE(print.config().enable_prime_tower.value);
        REQUIRE_FALSE(print.has_wipe_tower());
        CHECK_THAT(print.wipe_tower_data(1).depth, Catch::Matchers::WithinAbs(0., 1e-6));
    }

    SECTION("smooth timelapse prints one, and keeps enable_prime_tower on") {
        DynamicPrintConfig config = tower_estimate_config("rib", 1);
        config.set_deserialize_strict({ { "timelapse_type", "1" } });
        init_print({ cube(20) }, print, model, config);
        REQUIRE(print.has_wipe_tower());
        CHECK(print.wipe_tower_data(1).depth > 0.f);
    }
}

TEST_CASE("A tower printed without a tool change is still validated against the bed", "[WipeTower]")
{
    // Wrapping detection prints a tower on a plate that purges one filament. Neither the old
    // estimate (which read the wall type and smooth timelapse) nor the old containment gate (the
    // filament count or smooth timelapse) knew about it, so between them that tower was never
    // checked against the bed.
    Print print;
    Model model;
    DynamicPrintConfig config = tower_estimate_config("rectangle", 1);
    // Relative E without a per-layer G92 is rejected before the tower is ever looked at, and
    // has_wipe_tower() wants a real exclusion polygon before it honours wrapping detection.
    config.set_deserialize_strict({ { "enable_wrapping_detection", "1" },
                                    { "wrapping_exclude_area", "180x180,190x180,190x190,180x190" },
                                    { "wipe_tower_x", "500" }, { "wipe_tower_y", "500" },                                    { "use_relative_e_distances", "0" } });

    init_print({ cube(20) }, print, model, config);
    REQUIRE(print.extruders(true).size() == 1);
    REQUIRE(print.has_wipe_tower());
    CHECK(print.wipe_tower_data(1).depth > 0.f);
    CHECK_THAT(print.validate().string, Catch::Matchers::ContainsSubstring("printable area"));
}

// --- Multimaterial tower ---------------------------------------------------------------------
//
// prime_tower_multimaterial splits the tower footprint by filament instead of by tool change:
// one filament owns the outer shell ring, the other the inner core. The property that defines it
// is that the two filaments never share a part of the footprint, so neither is ever printed on
// top of the other. The stock tower does share it - the filament that prints the wall also fills
// the middle - which is what these tests tell the two layouts apart by.

// The band of the footprint one filament's extrusions cover on a tower layer, measured as the
// distance from the nearest tower edge (negative outside it, where the brim goes). The tower's
// own frame is turned half a circle every layer, and that maps the rectangle onto itself, so this
// places an extrusion in the shell or in the core whichever way up the layer is.
struct ToolBand
{
    float nearest  = std::numeric_limits<float>::max();
    float farthest = std::numeric_limits<float>::lowest();
};

static std::map<unsigned int, ToolBand> tower_layer_bands(const std::vector<WipeTower::ToolChangeResult> &layer,
                                                          float width, float depth)
{
    std::map<unsigned int, ToolBand> bands;
    for (const WipeTower::ToolChangeResult &tcr : layer)
        for (const WipeTower::Extrusion &extrusion : tcr.extrusions) {
            if (extrusion.width <= 0.f)
                continue; // a travel move, recorded to anchor the next extrusion
            const float distance = std::min(std::min(extrusion.pos.x(), width - extrusion.pos.x()),
                                            std::min(extrusion.pos.y(), depth - extrusion.pos.y()));
            ToolBand   &band     = bands[extrusion.tool];
            band.nearest         = std::min(band.nearest, distance);
            band.farthest        = std::max(band.farthest, distance);
        }
    return bands;
}

// A tower layer with no tool change on it, told apart the way WipeTowerIntegration does.
static bool tower_layer_is_sparse(const std::vector<WipeTower::ToolChangeResult> &layer)
{
    return layer.size() == 1 && layer.front().initial_tool == layer.front().new_tool;
}

// The filaments that extrude anything on a tower layer.
static std::set<unsigned int> tower_layer_filaments(const std::vector<WipeTower::ToolChangeResult> &layer)
{
    std::set<unsigned int> filaments;
    for (const WipeTower::ToolChangeResult &tcr : layer)
        for (const WipeTower::Extrusion &extrusion : tcr.extrusions)
            if (extrusion.width > 0.f)
                filaments.insert(extrusion.tool);
    return filaments;
}

// A per-layer tool change between the wall and the infill filaments, so every tower layer has
// both filaments on it and can be split into regions.
static DynamicPrintConfig multimaterial_tower_config(bool multimaterial)
{
    DynamicPrintConfig config = wipe_tower_toolchange_config("marlin");
    config.set_deserialize_strict({ { "wipe_tower_wall_type", "rectangle" },
                                    { "prime_volume", "45" },
                                    { "purge_in_prime_tower", "0" },
                                    { "single_extruder_multi_material", "0" },
                                    { "prime_tower_multimaterial", multimaterial ? "1" : "0" } });
    return config;
}

// Slices a 10mm cube and leaves the generated tower in `print`. The second apply is what
// slice_with_prime_tower() needs it for: the first one counts a single filament in use and
// normalize_fdm_2 would clear enable_prime_tower.
static void slice_prime_tower(const DynamicPrintConfig &config, Print &print, Model &model)
{
    init_print({ cube(10) }, print, model, config);
    print.apply(model, config);
    print.process();
    REQUIRE(print.is_step_done(psWipeTower));
}

TEST_CASE("The multimaterial prime tower gives each filament its own part of the footprint", "[WipeTower]")
{
    Print print;
    Model model;
    slice_prime_tower(multimaterial_tower_config(true), print, model);
    const WipeTowerData &data = print.wipe_tower_data();
    REQUIRE(data.width > 0.f);
    REQUIRE(data.depth > 0.f);

    std::set<unsigned int> shell_filaments;
    size_t                 split_layers = 0;

    for (const std::vector<WipeTower::ToolChangeResult> &layer : data.tool_changes) {
        if (tower_layer_is_sparse(layer))
            continue; // no tool change, so only one filament is available to print the layer
        const std::map<unsigned int, ToolBand> bands = tower_layer_bands(layer, data.width, data.depth);
        REQUIRE(bands.size() == 2);

        const ToolBand &first  = bands.begin()->second;
        const ToolBand &second = std::next(bands.begin())->second;
        // Whichever filament stays closer to the edge is the shell one.
        const bool first_is_shell = first.farthest < second.farthest;
        const ToolBand &shell = first_is_shell ? first : second;
        const ToolBand &core  = first_is_shell ? second : first;
        CHECK(shell.farthest < core.nearest);

        shell_filaments.insert((first_is_shell ? bands.begin() : std::next(bands.begin()))->first);
        ++split_layers;
    }

    REQUIRE(split_layers > 0);
    // The shell has to stay with one filament all the way up, or the regions would swap and the
    // materials would end up stacked on each other after all.
    CHECK(shell_filaments.size() == 1);
}

// Whether any two tower layers in a row are each printed by a single filament, and by a
// different one. That stacks one material on the other across the whole footprint, which is
// exactly what the multimaterial layout is there to avoid.
static bool tower_swaps_filament_between_whole_layers(const WipeTowerData &data)
{
    for (size_t i = 1; i < data.tool_changes.size(); ++i) {
        const std::set<unsigned int> below = tower_layer_filaments(data.tool_changes[i - 1]);
        const std::set<unsigned int> above = tower_layer_filaments(data.tool_changes[i]);
        if (below.size() == 1 && above.size() == 1 && *below.begin() != *above.begin())
            return true;
    }
    return false;
}

TEST_CASE("Turning the multimaterial prime tower off leaves the stock layout in place", "[WipeTower]")
{
    Print print;
    Model model;
    slice_prime_tower(multimaterial_tower_config(false), print, model);
    const WipeTowerData &data = print.wipe_tower_data();
    REQUIRE(data.tool_changes.size() > 1);

    // The stock tower gives a whole layer - purge, wall and sparse infill alike - to the filament
    // the layer's tool change brings in, so the footprint changes material from one layer to the
    // next. That is the behaviour the multimaterial layout replaces, and it has to survive the
    // option being off.
    CHECK(tower_swaps_filament_between_whole_layers(data));
}

TEST_CASE("The multimaterial prime tower never stacks one filament on the other", "[WipeTower]")
{
    Print print;
    Model model;
    slice_prime_tower(multimaterial_tower_config(true), print, model);
    CHECK_FALSE(tower_swaps_filament_between_whole_layers(print.wipe_tower_data()));
}

TEST_CASE("The multimaterial prime tower still purges the whole prime volume", "[WipeTower]")
{
    Print print;
    Model model;
    slice_prime_tower(multimaterial_tower_config(true), print, model);
    const WipeTowerData &data = print.wipe_tower_data();
    REQUIRE(data.number_of_toolchanges > 0);

    // prime_volume is the minimum a tool change has to purge. Sizing a region in whole loops and
    // whole rows can only round that up, so the tower may hold more, but never less.
    const double diameter = print.config().filament_diameter.get_at(0);
    const double area     = M_PI * diameter * diameter / 4.;
    double       volume   = 0.;
    for (float length : data.used_filament)
        volume += double(length) * area;
    CHECK(volume >= print.config().prime_volume.value * double(data.number_of_toolchanges));
}

// Filaments that extrude a brim on a layer WipeTowerIntegration would actually emit.
// The brim is the only part of the rectangular tower printed outside its footprint, so an
// extrusion at a negative distance from the tower edge is brim and nothing else.
// is_empty_wipe_tower_gcode drops every sparse layer when no_sparse_layers is on, including
// the plan's first entry - the one is_first_layer() points at.
static std::set<unsigned int> tower_printed_brim_filaments(const WipeTowerData &data, bool no_sparse)
{
    std::set<unsigned int> filaments;
    for (const std::vector<WipeTower::ToolChangeResult> &layer : data.tool_changes) {
        if (no_sparse && tower_layer_is_sparse(layer))
            continue;
        for (const auto &[filament, band] : tower_layer_bands(layer, data.width, data.depth))
            if (band.nearest < -0.2f)
                filaments.insert(filament);
    }
    return filaments;
}

TEST_CASE("The prime tower keeps its brim when sparse layers are skipped", "[WipeTower][Regression]")
{
    // First layers of this cube are one filament; the second only appears as sparse infill
    // higher up. Those first layers still enter the wipe-tower plan (partitions propagate
    // down), so they are sparse. G-code drops them when no_sparse_layers is on; the brim
    // has to land on the first layer that actually prints, or the tower has nothing holding
    // it to the bed.
    const bool multimaterial = GENERATE(false, true);
    DYNAMIC_SECTION("multimaterial tower " << (multimaterial ? "on" : "off")) {
        DynamicPrintConfig config = multimaterial_tower_config(multimaterial);
        config.set_deserialize_strict({
            { "wipe_tower_no_sparse_layers", "1" },
            { "top_surface_filament_id",     1 },
            { "bottom_surface_filament_id",  1 },
            { "outer_wall_filament_id",      1 },
            { "inner_wall_filament_id",      1 },
            { "internal_solid_filament_id",  1 },
            { "sparse_infill_filament_id",   2 },
        });

        Print print;
        Model model;
        slice_prime_tower(config, print, model);

        const WipeTowerData &data = print.wipe_tower_data();
        REQUIRE(data.tool_changes.size() > 1);
        REQUIRE(std::any_of(data.tool_changes.begin(), data.tool_changes.end(), tower_layer_is_sparse));
        REQUIRE(std::any_of(data.tool_changes.begin(), data.tool_changes.end(),
                            [](const std::vector<WipeTower::ToolChangeResult> &layer) {
                                return !tower_layer_is_sparse(layer);
                            }));

        const std::set<unsigned int> brim = tower_printed_brim_filaments(data, true);
        REQUIRE_FALSE(brim.empty());
        // One filament, the one whose walls the brim grows out of - a brim of the other material
        // would neither stick to those walls nor hold the tower down.
        CHECK(brim.size() == 1);

        const std::string gcode_str = gcode(print);
        CHECK_THAT(gcode_str, Catch::Matchers::ContainsSubstring("WIPE_TOWER_BRIM_START"));
    }
}

TEST_CASE("The multimaterial prime tower is rejected for more than two filaments", "[WipeTower]")
{
    // Three filaments would need nested rings; the generator only lays out the two regions, so
    // the combination is refused rather than silently falling back.
    DynamicPrintConfig config = multifilament_config(3, {
        { "sparse_infill_filament_id",  1 },
        { "internal_solid_filament_id", 1 },
        { "top_surface_filament_id",    3 },
        { "bottom_surface_filament_id", 3 },
        { "outer_wall_filament_id",     2 },
        { "inner_wall_filament_id",     2 },
        { "enable_prime_tower",         true },
        { "wipe_tower_wall_type",       "rectangle" },
        { "prime_tower_multimaterial",  true },
        { "wipe_tower_x",               50 },
        { "wipe_tower_y",               50 },
        { "layer_height",               0.3 } });

    Print print;
    Model model;
    init_print({ cube(10) }, print, model, config);
    print.apply(model, config);
    REQUIRE(print.extruders().size() == 3);
    REQUIRE(print.has_wipe_tower());
    CHECK_THAT(print.validate().string, Catch::Matchers::ContainsSubstring("exactly two filaments"));
}
