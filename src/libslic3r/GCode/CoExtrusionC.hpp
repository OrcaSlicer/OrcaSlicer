#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// Converts the tangent of an external perimeter and a co-extrusion color sector
// into a bounded C-axis position. The low-pass filter operates on the shortest
// angular distance, so crossing 0 degrees never filters through 180 degrees.
class CoExtrusionCController
{
public:
    void reset();

    std::optional<double> update_for_segment(double dx,
                                             double dy,
                                             bool   outward_normal_on_right,
                                             double color_sector_center,
                                             double calibration_offset,
                                             bool   reverse_axis,
                                             double filter_distance);

    static double normalize_angle(double angle);
    static double shortest_angular_delta(double from, double to);
    static double target_angle(double outward_normal,
                               double color_sector_center,
                               double calibration_offset,
                               bool   reverse_axis);

private:
    bool   m_initialized{false};
    double m_angle{0.0};
};

// Matches logical filament colors from a painted 3MF to physical color
// sectors. Explicit entries are one-based sector numbers; zero or missing
// entries use nearest-color matching. Unmappable entries contain size_t(-1).
std::vector<std::size_t> map_coextrusion_filament_colors_to_sectors(
    const std::vector<std::string> &filament_colors,
    const std::vector<std::string> &sector_colors,
    const std::vector<int>         &explicit_mapping = {});

} // namespace Slic3r
