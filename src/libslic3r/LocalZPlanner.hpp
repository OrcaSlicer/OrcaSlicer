// Pair-mix Local-Z (Subdivide Mix Layer) height split.
// Adapted from FullSpectrum_integration build_local_z_two_pass_heights, but
// clamps/refuses against machine min_layer_height (Ultra S 0.4 = 0.08 mm)
// instead of mixed_filament_height_lower_bound (often 0.04).
//
// If either computed pass is below min_layer_height, refuse the split
// (keep the whole nominal layer) rather than emit an illegal height.

#pragma once

#include "MixedFilament.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r {

struct LocalZPassHeights
{
    bool   split    { false };
    double height_a { 0.0 };
    double height_b { 0.0 };
};

inline std::pair<int, int> local_z_pair_ratio(const MixedFilament &mf)
{
    // Pattern rows keep M4 whole-layer cadence (FS excludes them from Local-Z).
    if (!mf.manual_pattern.empty())
        return { 0, 0 };
    // 3-component mixes stay whole-layer. Do not invent a 3-way sub-layer split.
    if (mf.component_c != 0)
        return { 0, 0 };
    unsigned ids[3];
    int      unique = 0;
    auto     push   = [&](unsigned id) {
        if (id == 0)
            return;
        for (int i = 0; i < unique; ++i) {
            if (ids[i] == id)
                return;
        }
        if (unique < 3)
            ids[unique++] = id;
    };
    push(mf.component_a);
    push(mf.component_b);
    if (mf.component_c != 0)
        push(mf.component_c);
    if (unique >= 3)
        return { 0, 0 };
    return { std::max(0, mf.ratio_a), std::max(0, mf.ratio_b) };
}

// Interpolate a pair ratio from 100:0 at the bottom to 0:100 at the top.
// z_frac is in [0, 1] (object-relative).
inline std::pair<int, int> interpolate_pair_ratio_by_z(double z_frac)
{
    const double t = std::clamp(z_frac, 0.0, 1.0);
    const int    rb = int(std::lround(t * 100.0));
    const int    ra = 100 - rb;
    return { ra, rb };
}

// Prefer 0.24 mm @ 2:1 → 0.16 + 0.08. Refuse 0.20 mm @ 2:1 below 0.08.
inline LocalZPassHeights plan_local_z_pair_heights(double base_height,
                                                   int    ratio_a,
                                                   int    ratio_b,
                                                   double min_layer_height)
{
    LocalZPassHeights out;
    out.height_a = base_height;
    if (!(base_height > 0.0) || !std::isfinite(base_height))
        return out;

    const int ra = std::max(0, ratio_a);
    const int rb = std::max(0, ratio_b);
    if (ra == 0 && rb == 0)
        return out;
    if (ra == 0 || rb == 0) {
        // Single-component layer: no split.
        return out;
    }

    const double min_h = std::max(0.0, min_layer_height);
    if (base_height + 1e-9 < 2.0 * min_h)
        return out;

    const double sum = double(ra + rb);
    double       h_a = base_height * (double(ra) / sum);
    double       h_b = base_height - h_a;

    // Honest clamp: do not lift a too-small pass up to min (that would steal
    // from the sibling and change the ratio). Refuse instead.
    if (h_a + 1e-9 < min_h || h_b + 1e-9 < min_h)
        return out;

    out.split    = true;
    out.height_a = h_a;
    out.height_b = h_b;
    return out;
}

inline LocalZPassHeights plan_local_z_pair_heights(double              base_height,
                                                   const MixedFilament &mf,
                                                   double              min_layer_height)
{
    const auto ratio = local_z_pair_ratio(mf);
    return plan_local_z_pair_heights(base_height, ratio.first, ratio.second, min_layer_height);
}

// Tool for a gradient layer that cannot split (endpoint 100:0 / 0:100,
// or a pass below min_layer_height). Dominant component wins; ties → A.
inline unsigned int gradient_fallback_extruder_1based(const MixedFilament &mf,
                                                      int                 ratio_a,
                                                      int                 ratio_b,
                                                      size_t              num_physical)
{
    const unsigned int a = (num_physical == 0) ? mf.component_a
                         : std::min(std::max(mf.component_a, 1u), unsigned(num_physical));
    const unsigned int b = (num_physical == 0) ? mf.component_b
                         : std::min(std::max(mf.component_b, 1u), unsigned(num_physical));
    if (ratio_b > ratio_a)
        return b;
    return a;
}

// Expand a nominal layer-height stack into rematerialized pass heights.
// First layer is held whole (elephant-foot / first-layer flow).
inline std::vector<double> rematerialize_layer_heights(const std::vector<double> &base_heights,
                                                       int                       ratio_a,
                                                       int                       ratio_b,
                                                       double                    min_layer_height)
{
    std::vector<double> out;
    out.reserve(base_heights.size() * 2);
    for (size_t i = 0; i < base_heights.size(); ++i) {
        if (i == 0) {
            out.push_back(base_heights[i]);
            continue;
        }
        const LocalZPassHeights h = plan_local_z_pair_heights(base_heights[i], ratio_a, ratio_b, min_layer_height);
        if (h.split) {
            out.push_back(h.height_a);
            out.push_back(h.height_b);
        } else {
            out.push_back(base_heights[i]);
        }
    }
    return out;
}

} // namespace Slic3r
