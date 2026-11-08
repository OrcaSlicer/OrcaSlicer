///|/ Wave overhang generation.
///|/
///|/ Wave overhangs algorithm: Janis A. Andersons (andersonsjanis).
///|/ Builds on arc-overhang algorithm by Steven McCulloch (stmcculloch).
///|/ PrusaSlicer integration: Steven McCulloch.
///|/ Port to OrcaSlicer: Dennis Klappe (dennisklappe).
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#ifndef slic3r_WaveOverhangs_hpp_
#define slic3r_WaveOverhangs_hpp_

#include <tuple>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::WaveOverhangs {

std::tuple<std::vector<ExtrusionPaths>, Polygons> generate(
    ExPolygons      infill_area,
    const Polygons &lower_slices_polygons,
    int             perimeter_count,
    int             additional_shell_count,
    double          wave_perimeter_overlap,
    double          minimum_wave_width,
    WaveOverhangPattern wave_pattern,
    double          wave_line_spacing,
    double          wave_line_width,
    const Flow     &overhang_flow,
    double          scaled_resolution,
    int             max_iterations            = 0,
    double          min_new_area_mm2          = 0.01,
    bool            use_instead_of_bridges    = false,
    // Corner-aware spacing taper. Master gate: corner_taper_enable=false skips
    // the taper entirely regardless of the other three values. When enabled
    // it also requires line_spacing_corner_mm < wave_line_spacing AND
    // corner_taper_distance_mm > 0 to actually emit denser corner fronts.
    bool            corner_taper_enable        = false,
    double          line_spacing_corner_mm    = 0.0,
    double          corner_taper_distance_mm  = 0.0,
    double          corner_angle_threshold_deg = 90.0);

} // namespace Slic3r::WaveOverhangs

#endif
