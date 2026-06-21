#ifndef slic3r_MagmaInjectionOrder_hpp_
#define slic3r_MagmaInjectionOrder_hpp_

#include "../Point.hpp"

#include <functional>
#include <vector>

namespace Slic3r {
namespace magma {

// Compute a visiting order over injection points (returns a permutation of
// indices into world_pts, which are scaled world-space XY).
//
//   spread_heat == false : travel-optimal order (chain_points TSP).
//   spread_heat == true  : CP-SAT order that separates spatially-near injections
//                          in time so combined heat doesn't melt neighbouring
//                          cells, warm-started from the TSP order (so it is never
//                          worse than TSP on the objective) and bounded by
//                          time_limit_s. Falls back to the TSP order on failure
//                          or on very large layers.
//
// This header intentionally exposes no OR-Tools types, so libslic3r consumers can
// include it while OR-Tools stays isolated in the magma_tube_solver library.
std::vector<size_t> order_injection_points(
    const Points& world_pts,
    bool spread_heat,
    double time_limit_s,
    const std::function<void()>& throw_if_canceled = {});

} // namespace magma
} // namespace Slic3r

#endif // slic3r_MagmaInjectionOrder_hpp_
