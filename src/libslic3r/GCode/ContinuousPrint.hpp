#ifndef slic3r_ContinuousPrint_hpp_
#define slic3r_ContinuousPrint_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../ShortestPath.hpp"

#include <utility>
#include <vector>

namespace Slic3r {

class ExtrusionEntity;
class Layer;
class PrintConfig;

// Preflight verdict of a single layer for the zero-travel continuous print mode.
enum class ContinuousPrintVerdict {
    Applicable,
    Reject,
};

// Single-chain plan of one layer, produced by the preflight and consumed by the emitter/filter.
struct ContinuousLayerPlan {
    // (index into the input entities, true if the entity must be reversed).
    std::vector<std::pair<size_t, bool>> order;
    Point start_point;                 // First point of the chain (XY).
    Point end_point;                   // Last point of the chain (XY). Coincides with start_point when closed.
    bool  is_closed = false;           // True if the chain forms a closed loop (start == end).
    double total_length = 0;           // Total extrusion length of the chain [mm], for the Z-ramp.
    std::vector<Point> sampling;       // XY samples along the chain, for the transition-curve / off-body checks.
};

// M1 scope: in-layer single-chain check only. The layer must be organizable into one continuous
// extrusion trace (an Eulerian trail, open or closed) without any travel or supplementary segments.
// Shape-level checks (single object / single material / no support / single island) and the
// transition-curve / off-body checks are deferred to M3 and will use `layer` and `cfg`.
// Returns Applicable and fills out_plan on success; Reject otherwise (out_plan untouched).
ContinuousPrintVerdict preflight_layer(
    const std::vector<ExtrusionEntity*> &entities,
    const Layer                         *layer,
    const PrintConfig                   &cfg,
    ContinuousLayerPlan                 *out_plan);

} // namespace Slic3r

#endif // slic3r_ContinuousPrint_hpp_
