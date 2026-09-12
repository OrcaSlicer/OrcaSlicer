#ifndef slic3r_ContinuousPrint_hpp_
#define slic3r_ContinuousPrint_hpp_

#include "../libslic3r.h"
#include "../ExtrusionEntity.hpp"
#include "../GCodeReader.hpp"
#include "../Point.hpp"
#include "../ShortestPath.hpp"
#include "SpiralVase.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace Slic3r {

class Layer;
class PrintConfig;

// Preflight verdict of a single layer for the zero-travel continuous print mode.
enum class ContinuousPrintVerdict {
    Applicable,
    Reject,
};

// Single-chain plan of one layer, produced by the preflight and consumed by the emitter/filter.
struct ContinuousLayerPlan {
    // Owned working set of entities (cloned from the input, possibly split at junctions,
    // see split_entities_at_junctions()). `order` below indexes into this vector.
    std::vector<std::unique_ptr<ExtrusionEntity>> entities;
    // (index into `entities`, true if the entity must be reversed).
    std::vector<std::pair<size_t, bool>> order;
    Point start_point;                 // First point of the chain (XY).
    Point end_point;                   // Last point of the chain (XY). Coincides with start_point when closed.
    bool  is_closed = false;           // True if the chain forms a closed loop (start == end).
    double total_length = 0;           // Total extrusion length of the chain [mm], for the Z-ramp.
    std::vector<Point> sampling;       // XY samples along the chain, for the transition-curve / off-body checks.
};

// Junction splitting (design doc 3.5, "lollipop" extension): whenever an endpoint of one entity
// lies on the interior of another entity (within junction_epsilon), split the latter at that point.
// This turns e.g. "wall loop + continuous infill trace touching the loop" into a graph whose
// Eulerian trail exists (2 odd-degree vertices), without creating any new geometry.
// Only ExtrusionPath and ExtrusionLoop entities are split; other entity types are passed through
// unchanged (junctions on them are not detected, which may lead to a Reject downstream).
// Returns the owned working set; entities without junctions are cloned 1:1 preserving input order.
std::vector<std::unique_ptr<ExtrusionEntity>> split_entities_at_junctions(
    const std::vector<ExtrusionEntity*> &entities,
    double junction_epsilon = SCALED_EPSILON);

// Recursively resolve ExtrusionEntityCollection nodes into their leaf entities (paths, loops,
// multipaths), preserving order. Leaf entities are appended to `out` (not owned).
void flatten_extrusion_entities(const ExtrusionEntity *entity, std::vector<ExtrusionEntity*> &out);
void flatten_extrusion_entities(const ExtrusionEntitiesPtr &entities, std::vector<ExtrusionEntity*> &out);

// M1 scope: in-layer single-chain check only. The layer must be organizable into one continuous
// extrusion trace (an Eulerian trail, open or closed) without any travel or supplementary segments.
// Shape-level checks (single object / single material / no support / single island) and the
// transition-curve / off-body checks are deferred to M3 and will use `layer` and `cfg`.
// `preferred_start` (optional) biases the chain start (and the linearization seam of closed loops)
// towards a given XY point, e.g. the end point of the previous layer.
// `junction_epsilon` (scaled units) is the tolerance within which an endpoint is considered to touch
// another trace. Real slicer output leaves a sub-line-width gap between wall and fill, so this must
// be a physical tolerance (e.g. half a nozzle diameter), not SCALED_EPSILON. Endpoints inside the
// tolerance are snapped onto the junction point, so no connecting line is added for them.
// `max_join_distance` (scaled units) is the longest straight connector the chainer may add between
// two traces (e.g. between two wall loops, or wall to support/fill). Longer hops make the layer
// unchainable. Set to 0 for strict "no connector" behaviour.
// Returns Applicable and fills out_plan on success; Reject otherwise (out_plan untouched).
ContinuousPrintVerdict preflight_layer(
    const std::vector<ExtrusionEntity*> &entities,
    const Layer                         *layer,
    const PrintConfig                   &cfg,
    ContinuousLayerPlan                 *out_plan,
    const Point                         *preferred_start = nullptr,
    double                               junction_epsilon = SCALED_EPSILON,
    double                               max_join_distance = 0.);

// Zero-travel continuous print filter (M2 prototype), generalized from SpiralVase:
// works on any layer emitted as a single continuous extrusion chain (open or closed),
// not only on a single perimeter loop. Reuses the same four mechanisms: initial Z-move
// rewrite, Z-ramp, smooth XY interpolation towards the previous layer, travel/retract
// filtering. The XY smoothing budget reuses spiral_mode_max_xy_smoothing (design doc 5.5).
// M3 hooks (not implemented yet): transition-point enforcement between layers and
// off-body checks of the smoothed transition segments.
class ContinuousPrint
{
public:
    explicit ContinuousPrint(const PrintConfig &config);

    void enable(bool en) {
        m_transition_layer = en && ! m_enabled;
        m_enabled          = en;
    }
    void set_max_xy_smoothing(float max) { m_max_xy_smoothing = max; }

    std::string process_layer(const std::string &gcode, bool last_layer);

private:
    const PrintConfig &m_config;
    GCodeReader m_reader;
    float       m_max_xy_smoothing = 0.f;

    bool m_enabled = false;
    // First continuous-print layer. Layer height has to be ramped up from zero to the target layer height.
    bool m_transition_layer = false;
    // Whether to interpolate XY coordinates with the previous layer.
    bool m_smooth = false;
    std::vector<SpiralVase::SpiralPoint> *m_previous_layer = nullptr;
};

} // namespace Slic3r

#endif // slic3r_ContinuousPrint_hpp_
