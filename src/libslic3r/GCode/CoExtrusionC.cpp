#include "CoExtrusionC.hpp"

#include "../Color.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

namespace {
constexpr double PI      = 3.14159265358979323846;
constexpr double EPSILON = 1e-9;
}

void CoExtrusionCController::reset()
{
    m_initialized = false;
    m_angle       = 0.0;
}

double CoExtrusionCController::normalize_angle(double angle)
{
    angle = std::fmod(angle, 360.0);
    if (angle < 0.0)
        angle += 360.0;
    // Avoid serializing 360 after floating point rounding at the boundary.
    return angle >= 360.0 ? 0.0 : angle;
}

double CoExtrusionCController::shortest_angular_delta(double from, double to)
{
    double delta = normalize_angle(to) - normalize_angle(from);
    if (delta > 180.0)
        delta -= 360.0;
    else if (delta <= -180.0)
        delta += 360.0;
    return delta;
}

double CoExtrusionCController::target_angle(double outward_normal,
                                             double color_sector_center,
                                             double calibration_offset,
                                             bool   reverse_axis)
{
    const double direction = reverse_axis ? -1.0 : 1.0;
    return normalize_angle(direction * (outward_normal - color_sector_center) + calibration_offset);
}

std::optional<double> CoExtrusionCController::update_for_segment(double dx,
                                                                  double dy,
                                                                  bool   outward_normal_on_right,
                                                                  double color_sector_center,
                                                                  double calibration_offset,
                                                                  bool   reverse_axis,
                                                                  double filter_distance)
{
    const double length = std::hypot(dx, dy);
    if (length <= EPSILON)
        return std::nullopt;

    const double normal_x = outward_normal_on_right ? dy : -dy;
    const double normal_y = outward_normal_on_right ? -dx : dx;
    const double normal   = std::atan2(normal_y, normal_x) * 180.0 / PI;
    const double target   = target_angle(normal, color_sector_center, calibration_offset, reverse_axis);

    if (!m_initialized || filter_distance <= EPSILON) {
        m_angle       = target;
        m_initialized = true;
    } else {
        const double alpha = 1.0 - std::exp(-length / filter_distance);
        m_angle = normalize_angle(m_angle + alpha * shortest_angular_delta(m_angle, target));
    }

    return m_angle;
}

std::vector<std::size_t> map_coextrusion_filament_colors_to_sectors(
    const std::vector<std::string> &filament_colors,
    const std::vector<std::string> &sector_colors,
    const std::vector<int>         &explicit_mapping)
{
    constexpr size_t unmapped = std::numeric_limits<size_t>::max();
    std::vector<size_t> result(filament_colors.size(), unmapped);
    if (filament_colors.empty() || sector_colors.empty())
        return result;

    struct Candidate {
        size_t filament;
        size_t sector;
        float  distance_squared;
    };

    std::vector<Candidate> candidates;
    for (size_t filament = 0; filament < filament_colors.size(); ++filament) {
        ColorRGB filament_color;
        if (!decode_color(filament_colors[filament], filament_color))
            continue;
        for (size_t sector = 0; sector < sector_colors.size(); ++sector) {
            ColorRGB sector_color;
            if (!decode_color(sector_colors[sector], sector_color))
                continue;
            const float dr = filament_color.r() - sector_color.r();
            const float dg = filament_color.g() - sector_color.g();
            const float db = filament_color.b() - sector_color.b();
            candidates.push_back({filament, sector, dr * dr + dg * dg + db * db});
        }
    }

    // A 3MF may contain more logical colors than the physical co-extruded
    // strand. Map every logical color independently to its nearest available
    // sector; multiple logical colors may intentionally share one sector.
    std::vector<float> best_distance(filament_colors.size(), std::numeric_limits<float>::infinity());
    for (const Candidate &candidate : candidates) {
        if (candidate.filament < explicit_mapping.size() && explicit_mapping[candidate.filament] > 0)
            continue;
        if (candidate.distance_squared < best_distance[candidate.filament]) {
            result[candidate.filament] = candidate.sector;
            best_distance[candidate.filament] = candidate.distance_squared;
        }
    }
    for (size_t filament = 0; filament < std::min(filament_colors.size(), explicit_mapping.size()); ++filament) {
        const int sector = explicit_mapping[filament];
        if (sector > 0 && size_t(sector) <= sector_colors.size())
            result[filament] = size_t(sector - 1);
    }
    return result;
}

} // namespace Slic3r
