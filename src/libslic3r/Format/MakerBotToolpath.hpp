#pragma once
// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// MakerBotToolpath.hpp
//
// G-code → Birdwing JSON Toolpath converter.
// Analysed from MakerBot Print 4.10.1 mb_toolpath_parser.js
//
// JSON Toolpath format (print.jsontoolpath in .makerbot archive):
//   Array of {function, parameters, tags} objects.
//   Coordinate origin: BUILD PLATE CENTER (not corner like G-code).
//   feedrate: mm/s (not mm/min).
//   'a': extruder A delta in mm of filament per move.

#include <string>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

struct BirdwingBuildVolume {
    double x { 300.0 };  // Z18
    double y { 305.0 };  // Z18
    double z { 457.0 };
    double layer_width { 0.4 };
};

// Convert Orca G-code file to Birdwing JSON toolpath string.
// Returns empty string on failure.
std::string gcode_to_birdwing_jsontoolpath(
    const std::string&        gcode_path,
    const BirdwingBuildVolume& bv,
    double                    layer_height,
    std::string&              error);

// Build the meta.json for a Birdwing .makerbot archive (no slip compensation).
std::string make_birdwing_meta_json(
    const std::string& bot_type,
    double             layer_height,
    double             layer_width,
    double             total_filament_mm,
    int                duration_s,
    const std::string& extruder_type    = "mk13",
    double             nozzle_diameter  = 0.4,
    double             feed_diameter    = 1.77,
    double             retract_distance = 0.5,
    double             extruder_temp    = 215.0,
    double             travel_speed_xy  = 150.0,
    double             travel_speed_z   = 3.0,
    double             fill_speed       = 110.0,
    double             inner_speed      = 90.0,
    double             outer_speed      = 40.0,
    bool               do_raft          = true,
    bool               do_fan           = true,
    bool               do_exp_decel     = true,
    double             retract_rate     = 30.0,   // aus Orca retraction_speed
    double             restart_rate     = 18.0);  // aus Orca deretraction_speed

} // namespace Slic3r
