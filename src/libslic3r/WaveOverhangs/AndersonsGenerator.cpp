///|/ Wave overhangs algorithm: Janis A. Andersons (andersonsjanis).
///|/ Builds on arc-overhang algorithm by Steven McCulloch (stmcculloch).
///|/ PrusaSlicer integration: Steven McCulloch.
///|/ Port to OrcaSlicer: Dennis Klappe (dennisklappe).
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#include "AndersonsGenerator.hpp"
#include "WaveOverhangs.hpp"

namespace Slic3r::WaveOverhangs {

GenerateResult AndersonsGenerator::generate(const ExPolygons   &overhang_area,
                                           const Polygons     &lower_slices_polygons,
                                           const CommonParams &params)
{
    // Filter out overhangs whose contour length is below the configured minimum.
    ExPolygons filtered;
    if (params.min_length_mm > 0.0) {
        const double min_len_scaled = scale_(params.min_length_mm);
        filtered.reserve(overhang_area.size());
        for (const ExPolygon &ex : overhang_area) {
            if (ex.contour.length() >= min_len_scaled)
                filtered.push_back(ex);
        }
    }
    const ExPolygons &src = (params.min_length_mm > 0.0) ? filtered : overhang_area;

    auto [paths, residual] = ::Slic3r::WaveOverhangs::generate(
        src,
        lower_slices_polygons,
        params.perimeter_count,
        params.additional_shell_count,
        params.perimeter_overlap,
        params.minimum_wave_width,
        params.pattern,
        params.line_spacing,
        params.line_width,
        params.overhang_flow,
        params.scaled_resolution,
        params.max_iterations,
        params.min_new_area,
        params.use_instead_of_bridges,
        params.corner_taper_enable,
        params.line_spacing_corner,
        params.corner_taper_distance,
        params.corner_angle_threshold);

    GenerateResult r;
    r.paths    = std::move(paths);
    r.residual = std::move(residual);
    return r;
}

} // namespace Slic3r::WaveOverhangs
