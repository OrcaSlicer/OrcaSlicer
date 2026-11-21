#ifndef slic3r_SketchSolver_hpp_
#define slic3r_SketchSolver_hpp_

// Bridge from the Design tab's SketchEntity / SketchEntityConstraintDef model onto the
// vendored SolveSpace constraint solver (src/libslic3r/slvs, libslvs). Replaces the
// hand-rolled SketchConstraints: full constraint set, real DoF counting, and
// over-constrained (bad-constraint) detection. Solves on a fixed 2D XY workplane.

#include <vector>

namespace Slic3r { enum class SketchPointRole; }
namespace Slic3r { struct SketchEntity; }
namespace Slic3r { struct SketchEntityConstraintDef; }

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

// While one of these is alive on a thread, solves skip the search for WHICH constraints conflict. That search is most of the
// cost of a solve that fails, or that finds a redundant (consistent) set, and a caller that only asks "did it solve?" throws
// the answer away: inferring relations on a drawn rectangle tried one redundant batch per add, ~100 ms each.
struct SketchSolveNoBad {
    SketchSolveNoBad();
    ~SketchSolveNoBad();
    SketchSolveNoBad(const SketchSolveNoBad&) = delete;
    SketchSolveNoBad& operator=(const SketchSolveNoBad&) = delete;
private:
    bool m_prev;
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

// Per-entity "still has a free degree of freedom" flags (1 = under-constrained, 0 = pinned down),
// one per entity. libslvs reports only the total DoF, so each entity is probed: pin everything it
// owns (its points, plus the radius of a circle) on a copy and see whether the total drops. A
// topology question, so callers cache it and recompute only when the entities or constraints
// change, not on every drag frame. Returns {} when the sketch does not solve.
std::vector<char> sketch_entity_free(const std::vector<SketchEntity>& entities,
                                     const std::vector<SketchEntityConstraintDef>& constraints);

// An over-constrained sketch the old libslvs can only call INCONSISTENT, redundant or conflicting alike. Each constraint of `bad`
// is told apart by taking it out: if the rest solve and the solved geometry still satisfies it, it was REDUNDANT (already implied);
// if it does not, it CONFLICTS with the others (or the rest do not solve either). At most 16 of `bad` are probed.
struct SketchOverReport {
    std::vector<int> redundant, conflicting;   // indices into `constraints`
};
SketchOverReport sketch_classify_bad(const std::vector<SketchEntity>& entities,
                                     const std::vector<SketchEntityConstraintDef>& constraints,
                                     const std::vector<int>& bad);

} // namespace Slic3r

#endif
