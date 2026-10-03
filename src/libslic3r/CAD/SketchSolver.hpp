#ifndef slic3r_SketchSolver_hpp_
#define slic3r_SketchSolver_hpp_

// Bridge from the Design tab's SketchEntity / SketchEntityConstraintDef model onto the
// vendored SolveSpace constraint solver (src/libslic3r/slvs, libslvs). Replaces the
// hand-rolled SketchConstraints: full constraint set, real DoF counting, and
// over-constrained (bad-constraint) detection. Solves on a fixed 2D XY workplane.

#include "libslic3r/CAD/SketchEngine.hpp"
#include <vector>

namespace Slic3r {

struct SketchSolveResult {
    bool             ok{false};   // solver converged & consistent
    int              dof{-1};     // remaining degrees of freedom (>0 under-constrained)
    int              result{0};   // raw SLVS_RESULT_* code
    std::vector<int> bad;         // indices (into `constraints`) of conflicting constraints
    // Indices of constraints that were NOT applied because an entity they reference has no
    // solver representation for the role they need (an ellipse rim, a spline curve, a
    // zero-radius circle...). A solve can be ok with some skipped; callers should say so.
    std::vector<int> skipped;
};

// Solve `constraints` over `entities` in place (writes solved coordinates back into the
// entities; arc angles are reflowed preserving sweep direction). No-op success when
// `constraints` is empty.
SketchSolveResult sketch_solve(std::vector<SketchEntity>& entities,
                               const std::vector<SketchEntityConstraintDef>& constraints);

// Drag-aware solve: pins the (dragged_ei, dragged_role) point's parameters via the
// solver's `dragged[]` priority list so the solver keeps that point where the cursor
// placed it (caller must have moved it first) and moves the OTHER free geometry to
// re-satisfy the constraints. dragged_ei < 0 behaves identically to sketch_solve.
SketchSolveResult sketch_solve_drag(std::vector<SketchEntity>& entities,
                                    const std::vector<SketchEntityConstraintDef>& constraints,
                                    int dragged_ei, SketchPointRole dragged_role);

} // namespace Slic3r

#endif
