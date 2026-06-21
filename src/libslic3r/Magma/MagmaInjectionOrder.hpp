#ifndef slic3r_MagmaInjectionOrder_hpp_
#define slic3r_MagmaInjectionOrder_hpp_

#include "../Point.hpp"

#include <functional>
#include <vector>

namespace Slic3r {
namespace magma {

// Per-print timing model for the injection phase, used to convert the visiting
// order into real elapsed seconds so the heat decay is physically grounded
// (a far jump costs travel time, but that time is also cooling). All values are
// representative scalars read once from the print config.
struct InjectionTiming {
    double travel_speed_mm_s    = 150.0;  // XY travel speed between injections
    double per_injection_fixed_s = 0.0;   // ~constant per-injection time: z-hops + dwell (+ slam)
    double vol_speed_mm3_s      = 10.0;   // volumetric injection rate (extrude time = volume / rate)
};

// Compute a visiting order over injection points (returns a permutation of
// indices into world_pts, which are scaled world-space XY).
//
//   spread_heat == false : travel-optimal order (chain_points TSP).
//   spread_heat == true  : heat-spread order. A time-decay dispersion greedy
//                          builds an order that keeps spatially-near injections
//                          far apart in real time, then a violation-directed
//                          local search polishes it. Deterministic, O(n^2), no
//                          external solver (CP-SAT was measured here and could
//                          not beat greedy+polish, so it was dropped).
//
// injected_volume_mm3 is parallel to world_pts (effective injected volume per
// point, fill-factor applied), used to put the heat decay on a real-seconds clock.
std::vector<size_t> order_injection_points(
    const Points& world_pts,
    const std::vector<double>& injected_volume_mm3,
    const InjectionTiming& timing,
    bool spread_heat,
    const std::function<void()>& throw_if_canceled = {});

} // namespace magma
} // namespace Slic3r

#endif // slic3r_MagmaInjectionOrder_hpp_
