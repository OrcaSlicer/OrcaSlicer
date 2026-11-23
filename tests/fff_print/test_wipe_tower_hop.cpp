#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "libslic3r/GCode/GCodeProcessor.hpp"

#include "test_helpers.hpp"

// The Z-hop shape of the travel that enters a prime-tower tool change.
//
// GCode.cpp picks LiftType::NormalLift for the tower travel unless the writer position is
// clear, and the per-layer timelapse / wrapping insertions run just before the tower
// toolchange, so an inert block that clears the position changes that hop. This slices a
// two-extruder print with a prime tower and classifies the travel ahead of each tool change.

namespace {

using namespace Slic3r;
using namespace Test;

DynamicPrintConfig tower_config(const std::string & time_lapse_gcode)
{
    // Walls on filament 2, infill on filament 1: the head visits the prime tower on every layer.
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
        // Not the setting under test here; keep the travel planner out of the picture.
        { "reduce_crossing_wall",       false },
        { "z_hop",                      0.4 },
        { "z_hop_types",                "Spiral Lift" },
        { "time_lapse_gcode",           time_lapse_gcode },
    });
}

// Shape of the moves between the last extrusion and each prime-tower region.
struct TowerHops {
    size_t tower_regions = 0;
    size_t toolchanges   = 0;   // extruder words / BBL markers, for diagnosis
    size_t spiral        = 0;   // an arc with Z: the spiral lift ran
    size_t z_then_xy     = 0;   // a Z-only move and a separate XY move: the standing lift
    size_t single_xyz    = 0;   // one move carrying XY and Z
    size_t plain_xy      = 0;   // no Z at all: no hop
    size_t no_travel     = 0;
};

bool has_word(const std::string & line, const std::string & word)
{
    size_t p = line.find(word);
    return p != std::string::npos && (p == 0 || line[p - 1] == ' ');
}

TowerHops analyse(const std::string & gcode)
{
    const std::string & start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start);
    TowerHops                      out;
    std::vector<std::string>       tail;
    auto flush_window = [&](void) {
        ++out.tower_regions;
        bool saw_xy = false;
        bool saw_z_only = false;
        bool saw_xyz = false;
        bool saw_arc_z = false;
        for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
            const std::string & l = *it;
            const bool          g1 = l.rfind("G1", 0) == 0 || l.rfind("G0", 0) == 0;
            const bool          arc = l.rfind("G2", 0) == 0 || l.rfind("G3", 0) == 0;
            if (!g1 && !arc)
                continue;
            const bool xy = has_word(l, "X") || has_word(l, "Y");
            const bool z  = has_word(l, "Z");
            const bool e  = has_word(l, "E");
            if (e)
                break;                       // the block ends at the last extrusion
            if (arc && z)
                saw_arc_z = true;
            if (xy && z)
                saw_xyz = true;
            else if (xy)
                saw_xy = true;
            else if (z)
                saw_z_only = true;
            if (xy)
                break;                       // the travel into the tool change
        }
        if (saw_arc_z)
            ++out.spiral;
        else if (saw_z_only && (saw_xy || saw_xyz))
            ++out.z_then_xy;
        else if (saw_xyz)
            ++out.single_xyz;
        else if (saw_xy)
            ++out.plain_xy;
        else
            ++out.no_travel;
        tail.clear();
    };

    std::istringstream in(gcode);
    std::string        line;
    while (std::getline(in, line)) {
        if (line.find(start_tag) != std::string::npos)
            flush_window();
        std::string s = line;
        s.erase(std::find(s.begin(), s.end(), ';'), s.end());
        while (!s.empty() && s.back() == ' ')
            s.pop_back();
        if (s.empty())
            continue;
        // A tool change for the flavours used here: an extruder word, or the BBL markers.
        if ((s.size() > 1 && s[0] == 'T' && isdigit(s[1])) ||
            (s.rfind("M620", 0) == 0) || (s.rfind("M621", 0) == 0))
            ++out.toolchanges;
        if (s[0] == 'G' || s[0] == 'M')
            tail.push_back(s);
        if (tail.size() > 40)
            tail.erase(tail.begin());
    }
    return out;
}

// Two wall-only cubes, the second on extruder 2, sliced by layer: the head returns to the
// prime tower at every layer change.
TowerHops slice_two_extruders(const std::string & time_lapse_gcode)
{
    const DynamicPrintConfig config = tower_config(time_lapse_gcode);
    Print                    print;
    Model                    model;
    // Not plain Test::slice: the first apply of a brand-new Print counts one filament in use and
    // normalize_fdm_2's single-filament rule clears `enable_prime_tower`. Apply again once the
    // regions have settled, so the tower survives.
    init_print({ cube(10) }, print, model, config);
    print.apply(model, config);
    // The block is inserted at the layer change of the Bambu Lab machines.
    print.is_BBL_printer() = true;
    const std::string gcode = Test::gcode(print);
    INFO(gcode.substr(0, std::min<size_t>(gcode.size(), 4000)));
    return analyse(gcode);
}

// A prime-tower region was sliced, and its entry travel is classified.
bool same_shape(const TowerHops & a, const TowerHops & b)
{
    return a.spiral == b.spiral && a.z_then_xy == b.z_then_xy && a.single_xyz == b.single_xyz &&
           a.plain_xy == b.plain_xy && a.no_travel == b.no_travel;
}

} // namespace

TEST_CASE("Prime-tower entry travel does not depend on a per-layer block moving the toolhead", "[WipeTower][GCode][AvoidCrossingWalls]")
{
    // The block that commands no motion, and one that does: the writer position is clear for the
    // first, unknown for the second, and GCode.cpp:1721 picks the tower hop from that flag.
    const TowerHops inert  = slice_two_extruders("; take a photo\nM1002 judge_flag timelapse_record_flag\nM400 P300\n");
    const TowerHops moving = slice_two_extruders("; photo move\nG1 X0 Y0 F18000\nM400 P300\nG1 X0 F18000\n");
    INFO("inert:  regions=" << inert.tower_regions << " spiral=" << inert.spiral
         << " z_then_xy=" << inert.z_then_xy << " single_xyz=" << inert.single_xyz
         << " plain_xy=" << inert.plain_xy << " no_travel=" << inert.no_travel);
    INFO("moving: regions=" << moving.tower_regions << " spiral=" << moving.spiral
         << " z_then_xy=" << moving.z_then_xy << " single_xyz=" << moving.single_xyz
         << " plain_xy=" << moving.plain_xy << " no_travel=" << moving.no_travel);
    // The tower runs on every layer change of this configuration.
    REQUIRE(inert.tower_regions >= 3);
    REQUIRE(moving.tower_regions >= 3);
    // Same number of tower entries, and the same hop shape for each of them.
    CHECK(inert.tower_regions == moving.tower_regions);
    CHECK(same_shape(inert, moving));
}
