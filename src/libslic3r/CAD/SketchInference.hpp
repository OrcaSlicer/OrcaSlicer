#ifndef slic3r_SketchInference_hpp_
#define slic3r_SketchInference_hpp_

#include "libslic3r/CAD/SketchEngine.hpp"
#include <vector>
#include <optional>
#include <cmath>

namespace Slic3r {

// Result of snapping a free cursor point onto the most relevant inference target
// among the committed sketch entities and the sketch origin. This is the backbone
// that lets geometry self-constrain as it is drawn: the GUI records the returned
// target at click time and, once the entity it belongs to exists, emits the
// matching constraint (Coincident onto an endpoint/centre, Fix onto the origin,
// PointOnObject onto an edge) so the relation survives a re-solve.
struct InferenceSnap {
    enum class Kind { None, Endpoint, Center, Midpoint, OnEdge, Origin };
    Kind            kind{Kind::None};
    int             entity{-1};                       // hit entity index (-1 = origin/none)
    SketchPointRole role{SketchPointRole::P0};         // which point of `entity` (Endpoint/Center)
    Vec2d           point{0, 0};                       // snapped coordinate (== query when None)

    bool snapped() const { return kind != Kind::None; }
};

// Snap `query` onto the best inference target within `tol` plane units. Priority,
// highest first: Endpoint, Center, Origin, Midpoint, OnEdge. Construction entities
// participate (you constrain to them too). Returns {None, query} when nothing is in
// range. Pure — no GUI / GL dependencies, so it is unit-testable in libslic3r.
InferenceSnap infer_point_snap(const std::vector<SketchEntity>& entities,
                               const Vec2d& query, double tol,
                               bool include_origin = true);

// Relational inference for an in-progress segment anchor->tip. If its direction is
// within `ang_tol_rad` of an axis, returns Horizontal or Vertical (the constraint to
// auto-emit on the committed segment); std::nullopt otherwise. Degenerate (near-zero
// length) segments return nullopt.
std::optional<SketchConstraintType>
infer_axis_constraint(const Vec2d& anchor, const Vec2d& tip, double ang_tol_rad = 3.0 * M_PI / 180.0);

} // namespace Slic3r

#endif // slic3r_SketchInference_hpp_
