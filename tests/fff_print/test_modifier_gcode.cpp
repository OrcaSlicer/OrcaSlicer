#include <catch2/catch_all.hpp>

#include <algorithm>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Count of (non-overlapping) occurrences of `needle` in `haystack`.
int count_occurrences(const std::string &haystack, const std::string &needle)
{
    int count = 0;
    for (auto pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

// Builds a 20mm base cube plus an overlapping modifier volume (also cuboid, placed at the
// origin so its raw mesh coordinates directly define its footprint/extents within the object,
// per make_cube()'s (0,0,0)-(x,y,z) convention) carrying the given enter/exit G-code.
void slice_cube_with_modifier(Vec3d modifier_dims, Vec3d modifier_offset,
                               const std::string &enter_gcode, const std::string &exit_gcode,
                               std::string &out_gcode, bool group_together = false)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               2 },
        { "initial_layer_print_height", 2 },
    });

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20), ModelVolumeType::MODEL_PART, false);

    ModelVolume *modifier = object->add_volume(make_cube(modifier_dims.x(), modifier_dims.y(), modifier_dims.z()),
                                                ModelVolumeType::PARAMETER_MODIFIER, false);
    modifier->set_offset(modifier_offset);
    modifier->config.set_key_value("modifier_enter_gcode", new ConfigOptionString(enter_gcode));
    modifier->config.set_key_value("modifier_exit_gcode", new ConfigOptionString(exit_gcode));
    if (group_together)
        modifier->config.set_key_value("modifier_group_together", new ConfigOptionBool(true));

    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);

    out_gcode = Slic3r::Test::gcode(print);
}

} // namespace

TEST_CASE("Modifier region emits balanced enter/exit G-code on every layer it spans", "[ModifierGCode]") {
    // 10mm modifier cube at the object's bottom corner, coincident with two of the base cube's
    // own edges: the (merged, continuous) outer wall loop dips into and out of the modifier's
    // footprint as it wraps the corner, crossing its boundary twice per layer, on each of the
    // layers 1..5 (print_z 2,4,6,8,10) the modifier spans. No other setting differs from the
    // base object, so this also regression-tests that a G-code-only modifier still gets its
    // G-code bracketed even though its perimeter generation is merged with its parent's.
    std::string gcode;
    slice_cube_with_modifier(Vec3d(10, 10, 10), Vec3d(0, 0, 0), "; ENTER_MOD\n", "; EXIT_MOD\n", gcode);

    const int enter_count = count_occurrences(gcode, "; ENTER_MOD");
    const int exit_count  = count_occurrences(gcode, "; EXIT_MOD");

    CHECK(enter_count > 1); // fires again each time the toolpath re-enters the region, not once for the whole volume
    CHECK(exit_count > 1);
    CHECK(enter_count == exit_count); // every enter is matched by an exit
}

TEST_CASE("Grouped modifier's infill prints as one contiguous block per layer, walls unaffected", "[ModifierGCode]") {
    // 5x5x10 modifier centered in X/Y (7.5mm inset from every face) and spanning the object's
    // bottom 10mm: far enough from the outer surface that the merged, continuous wall loop never
    // comes near its boundary, so crossing detection contributes zero wall-based firings here —
    // isolating the property under test. With modifier_group_together set, every marker should
    // come from the single deferred infill block per layer (5 layers spanned, at print_z
    // 2,4,6,8,10), not from repeated infill-line crossings the way ungrouped infill would fire.
    std::string gcode;
    slice_cube_with_modifier(Vec3d(5, 5, 10), Vec3d(7.5, 7.5, 0), "; ENTER_MOD\n", "; EXIT_MOD\n", gcode, /*group_together=*/true);

    const int enter_count = count_occurrences(gcode, "; ENTER_MOD");
    const int exit_count  = count_occurrences(gcode, "; EXIT_MOD");

    CHECK(enter_count == 5);
    CHECK(exit_count == 5);

    // No layer outside the modifier's Z range gets a (spurious, empty) bracket.
    const int last_enter_pos = static_cast<int>(gcode.rfind("; ENTER_MOD"));
    const int last_exit_pos  = static_cast<int>(gcode.rfind("; EXIT_MOD"));
    CHECK(last_enter_pos >= 0);
    CHECK(last_exit_pos > last_enter_pos);
}

TEST_CASE("Grouping a modifier's infill does not add wall geometry at its boundary", "[ModifierGCode]") {
    // Same corner modifier used in the ungrouped test above (touching two of the base cube's own
    // edges, so its boundary does intersect the continuous outer wall loop) — this time with
    // modifier_group_together set. Walls must still be governed purely by crossing detection
    // (unaffected by grouping): the same ">1 fire, balanced" shape as the ungrouped case, since
    // is_perimeter_compatible() no longer differentiates on modifier_group_together at all.
    std::string gcode;
    slice_cube_with_modifier(Vec3d(10, 10, 10), Vec3d(0, 0, 0), "; ENTER_MOD\n", "; EXIT_MOD\n", gcode, /*group_together=*/true);

    const int enter_count = count_occurrences(gcode, "; ENTER_MOD");
    const int exit_count  = count_occurrences(gcode, "; EXIT_MOD");

    // Wall crossings (still per-crossing) plus one deferred infill block per layer.
    CHECK(enter_count > 5);
    CHECK(exit_count > 5);
    CHECK(enter_count == exit_count);
}

TEST_CASE("Modifier still active at the end of the print still emits its exit G-code", "[ModifierGCode]") {
    // Modifier is a thin slab covering the object's full footprint for its top 5mm (15mm-20mm):
    // the wall loop is "inside" it for the entirety of every layer it spans, so no crossing
    // occurs within those layers (enter fires once, via the resync at the first wall path whose
    // start point is newly inside the boundary). Without a trailing flush at export end, that
    // single "enter" would have no matching "exit" appended after it.
    std::string gcode;
    slice_cube_with_modifier(Vec3d(20, 20, 5), Vec3d(0, 0, 15), "; ENTER_MOD\n", "; EXIT_MOD\n", gcode);

    const int enter_count = count_occurrences(gcode, "; ENTER_MOD");
    const int exit_count  = count_occurrences(gcode, "; EXIT_MOD");

    CHECK(enter_count > 0);
    CHECK(enter_count == exit_count);
}

TEST_CASE("Modifier exit G-code is optional", "[ModifierGCode]") {
    // Only enter G-code configured: exit should simply be skipped (empty template -> no emission),
    // not crash or emit a stray marker.
    std::string gcode;
    slice_cube_with_modifier(Vec3d(10, 10, 10), Vec3d(0, 0, 0), "; ENTER_MOD\n", "", gcode);

    CHECK(count_occurrences(gcode, "; ENTER_MOD") > 0);
    CHECK(count_occurrences(gcode, "; EXIT_MOD") == 0);
}

TEST_CASE("Modifier enter/exit G-code never leaks across a layer change", "[ModifierGCode]") {
    // If a wall loop's last segment ends a layer while still geometrically inside the modifier's
    // boundary (no further crossing before the Z move to the next layer), that "inside" state must
    // be flushed for THIS layer rather than silently carried into the next one's resync check —
    // otherwise the dangling exit is dropped, and worse, a stale "inside" flag can desync whatever
    // a later, unrelated resync decides for a different modifier. layer_change_gcode marks every
    // layer boundary, so the enter/exit count can be checked for balance within each layer segment
    // independently, not just across the whole G-code (where a later self-correction could hide
    // a same-layer leak).
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               2 },
        { "initial_layer_print_height", 2 },
        { "layer_change_gcode",         "; LAYER_CHANGE\n" },
    });

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20), ModelVolumeType::MODEL_PART, false);

    ModelVolume *modifier = object->add_volume(make_cube(10, 10, 10), ModelVolumeType::PARAMETER_MODIFIER, false);
    modifier->set_offset(Vec3d(0, 0, 0));
    modifier->config.set_key_value("modifier_enter_gcode", new ConfigOptionString("; ENTER_MOD\n"));
    modifier->config.set_key_value("modifier_exit_gcode", new ConfigOptionString("; EXIT_MOD\n"));

    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);

    std::string gcode = Slic3r::Test::gcode(print);
    REQUIRE(count_occurrences(gcode, "; ENTER_MOD") > 0); // sanity: the modifier is actually exercised

    size_t pos = 0;
    int checked_segments = 0;
    for (;;) {
        size_t next = gcode.find("; LAYER_CHANGE", pos);
        std::string segment = gcode.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        CHECK(count_occurrences(segment, "; ENTER_MOD") == count_occurrences(segment, "; EXIT_MOD"));
        ++checked_segments;
        if (next == std::string::npos)
            break;
        pos = next + std::string("; LAYER_CHANGE").size();
    }
    CHECK(checked_segments > 1); // sanity: the print actually spans multiple layers
}

TEST_CASE("Touching modifiers close the first region before opening the second", "[ModifierGCode]") {
    // Two 5x10x10 modifiers side by side (touching along their shared X=5 plane, not
    // overlapping), together forming the same 10x10x10 corner used in the tests above. As the
    // continuous wall loop passes through this corner, it crosses the shared A/B boundary: the
    // exit from whichever region it's leaving must be emitted before the enter into the other,
    // even though the two crossings are computed independently (against two different polygons)
    // at the same physical point, where floating-point noise could otherwise flip their order.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               2 },
        { "initial_layer_print_height", 2 },
    });

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20), ModelVolumeType::MODEL_PART, false);

    ModelVolume *a = object->add_volume(make_cube(5, 10, 10), ModelVolumeType::PARAMETER_MODIFIER, false);
    a->set_offset(Vec3d(0, 0, 0));
    a->config.set_key_value("modifier_enter_gcode", new ConfigOptionString("; ENTER_A\n"));
    a->config.set_key_value("modifier_exit_gcode", new ConfigOptionString("; EXIT_A\n"));

    ModelVolume *b = object->add_volume(make_cube(5, 10, 10), ModelVolumeType::PARAMETER_MODIFIER, false);
    b->set_offset(Vec3d(5, 0, 0));
    b->config.set_key_value("modifier_enter_gcode", new ConfigOptionString("; ENTER_B\n"));
    b->config.set_key_value("modifier_exit_gcode", new ConfigOptionString("; EXIT_B\n"));

    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);

    std::string gcode = Slic3r::Test::gcode(print);

    // Collect every marker occurrence, in the order it appears in the G-code.
    struct Event { size_t pos; bool is_a; bool entering; };
    std::vector<Event> events;
    auto collect = [&](const std::string &needle, bool is_a, bool entering) {
        for (auto pos = gcode.find(needle); pos != std::string::npos; pos = gcode.find(needle, pos + needle.size()))
            events.push_back({pos, is_a, entering});
    };
    collect("; ENTER_A", true, true);
    collect("; EXIT_A", true, false);
    collect("; ENTER_B", false, true);
    collect("; EXIT_B", false, false);
    std::sort(events.begin(), events.end(), [](const Event &l, const Event &r) { return l.pos < r.pos; });

    REQUIRE(!events.empty());

    // A and B are non-overlapping: replaying the events in G-code order must never show the
    // toolpath as simultaneously "inside" both — that's exactly the bug shape (enterA, enterB,
    // exitA, exitB) reported when the shared-boundary crossings sorted the wrong way round.
    bool inside_a = false, inside_b = false;
    for (const Event &e : events) {
        if (e.is_a)
            inside_a = e.entering;
        else
            inside_b = e.entering;
        CHECK_FALSE(inside_a && inside_b);
    }
}
