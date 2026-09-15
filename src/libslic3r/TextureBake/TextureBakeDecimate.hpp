#pragma once

// Quadric error metric decimation (Garland & Heckbert), with two additions that matter on a
// displaced mesh.
//
// Crease quadrics: an interior edge sharper than the threshold gets penalty planes at both endpoints,
// perpendicular to each adjacent face and through the edge, weighted so such edges collapse last or
// not at all. A texture's hard step keeps its geometry while the flat ground around it reduces.
//
// Flat-face harvesting: the loop keeps going past the triangle target while each collapse's error
// stays under an absolute bound, so flat faces that cost nothing to remove are not left behind.

#include <cstdint>
#include <functional>
#include <vector>

#include "TextureBakeIndex.hpp"

namespace Slic3r {
namespace TextureBake {

// Reject a collapse deviating more than about 78 degrees from the old face normal.
static constexpr double DECIMATE_FLIP_DOT = 0.2;
// Edges sharper than 60 degrees are treated as creases.
static constexpr double DECIMATE_CREASE_COS = 0.5;
// Quadric penalty weight for a crease plane.
static constexpr double DECIMATE_CREASE_WEIGHT = 1e4;

// Upper bound in mm on the deviation a harvested collapse may introduce; the real one is smaller,
// since the cost sums squared distances over all incident faces.
//
// Absolute, not relative to the cost at which the target was crossed. A relative band fails in the
// case with the most to shed: when the target is reached with a large flat surplus left, the crossing
// cost is essentially zero, so the band is too and nothing is harvested.
static constexpr double DECIMATE_DEFAULT_HARVEST_TOL = 0.005;

// Returns false to cancel.
using DecimateProgressFn = std::function<bool(double fraction)>;

struct DecimateResult
{
    TriSoup geometry;
    // The locked faces alone met the target, so it was unreachable without touching preserved
    // geometry.
    bool locked_over_budget = false;
};

// `locked_faces`: one entry per input triangle; a vertex touching one may neither move nor be
// removed, which also pins the ring between the two regions.
DecimateResult decimate(const TriSoup &geometry, size_t target_triangles, bool harvest_flat = true,
                        double harvest_tol = DECIMATE_DEFAULT_HARVEST_TOL,
                        const std::vector<uint8_t> &locked_faces = {},
                        const DecimateProgressFn   &on_progress  = {});

} // namespace TextureBake
} // namespace Slic3r
