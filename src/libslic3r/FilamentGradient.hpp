#pragma once

#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include "Color.hpp"

namespace Slic3r {

// A single color stop in a gradient, position is in [0, 1] along one cycle.
struct GradientColorStop {
    float     position { 0.0f };  // 0.0 = cycle start, 1.0 = cycle end
    ColorRGBA color;

    bool operator<(const GradientColorStop& other) const { return position < other.position; }
};

// Describes a repeating filament color gradient.
// The gradient repeats every cycle_length_mm of extruded filament.
// start_offset_mm shifts where in the cycle the spool begins.
struct FilamentGradient {
    std::vector<GradientColorStop> stops;
    float cycle_length_mm  { 500.0f };
    float start_offset_mm  { 0.0f };

    bool empty() const { return stops.size() < 2; }

    // Returns the interpolated color at a given cumulative extrusion distance.
    ColorRGBA sample(float extruded_mm) const
    {
        if (empty())
            return ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);

        const float len = cycle_length_mm > 0.0f ? cycle_length_mm : 1.0f;
        float pos = std::fmod(extruded_mm + start_offset_mm, len) / len;
        if (pos < 0.0f) pos += 1.0f;

        // Find the two stops that bracket pos.
        // stops is kept sorted by position.
        auto it = std::lower_bound(stops.begin(), stops.end(),
                                   GradientColorStop{ pos, {} });

        if (it == stops.end())
            return stops.back().color;
        if (it == stops.begin())
            return stops.front().color;

        const GradientColorStop& hi = *it;
        const GradientColorStop& lo = *std::prev(it);

        const float range = hi.position - lo.position;
        const float t     = (range > 0.0f) ? (pos - lo.position) / range : 0.0f;

        return lo.color * (1.0f - t) + hi.color * t;
    }

    // Serialize stops to a compact string: "pos1,#RRGGBBAA;pos2,#RRGGBBAA;..."
    std::string serialize_stops() const;

    // Parse stops from the format produced by serialize_stops().
    // Returns false if the string is malformed.
    static bool parse_stops(const std::string& s, std::vector<GradientColorStop>& out);

    // Build evenly-spaced stops from a list of colors (space- or comma-separated hex,
    // e.g. "#RRGGBB #RRGGBB ..."), as used by OrcaSlicer's native filament_multi_colour.
    // Returns false if fewer than 2 valid colors are found.
    static bool stops_from_color_list(const std::string& s, std::vector<GradientColorStop>& out);
};

} // namespace Slic3r
