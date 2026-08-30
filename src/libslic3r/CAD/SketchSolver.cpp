#include "libslic3r/CAD/SketchSolver.hpp"

#include <slvs.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>

namespace Slic3r {

using CT = SketchConstraintType;
using Role = SketchPointRole;

namespace {

constexpr Slvs_hGroup G_FIXED = 1;   // workplane / reference: held constant
constexpr Slvs_hGroup G_SK    = 2;   // sketch geometry: the group we solve

// Per-entity slvs handles. p0/p1/center are point2d entity handles; prim is the
// line/arc/circle entity; rparam is the circle radius param.
struct Slots {
    Slvs_hEntity prim{0}, p0{0}, p1{0}, center{0};
    Slvs_hParam  rparam{0};
    std::vector<Slvs_hEntity> pts;   // BSpline control points (point2d handles)
};

struct Build {
    std::vector<Slvs_Param>      params;
    std::vector<Slvs_Entity>     ents;
    std::vector<Slvs_Constraint> cons;
    Slvs_hParam      ph{0};
    Slvs_hEntity     eh{0};
    Slvs_hConstraint ch{0};
    Slvs_hEntity     wp{0}, normal{0};

    Slvs_hParam  P(Slvs_hGroup g, double v) { params.push_back(Slvs_MakeParam(++ph, g, v)); return ph; }
    Slvs_hEntity E(Slvs_Entity e)           { ents.push_back(e); return e.h; }
    Slvs_hEntity pt2d(Slvs_hGroup g, double u, double v)
        { return E(Slvs_MakePoint2d(++eh, g, wp, P(g, u), P(g, v))); }

    // Generic constraint (entityC unused by Slvs_MakeConstraint — set it manually below).
    void C(int type, double val, Slvs_hEntity ptA, Slvs_hEntity ptB,
           Slvs_hEntity eA, Slvs_hEntity eB, Slvs_hEntity eC = 0, int other = 0)
    {
        Slvs_Constraint c = Slvs_MakeConstraint(++ch, G_SK, type, wp, val, ptA, ptB, eA, eB);
        c.entityC = eC;
        c.other   = other;
        cons.push_back(c);
    }
};

inline int role_idx(Role r) { return int(r); }

} // namespace

static SketchSolveResult solve_system(std::vector<SketchEntity>& entities,
                                      const std::vector<SketchEntityConstraintDef>& constraints,
                                      int dragged_ei, Role dragged_role)
{
    SketchSolveResult out;
    if (constraints.empty()) { out.ok = true; out.dof = -1; return out; }

    Build b;

    // ---- Fixed 2D XY workplane (origin at 0,0,0; identity normal) -------------------
    Slvs_hEntity origin = b.E(Slvs_MakePoint3d(++b.eh, G_FIXED,
                              b.P(G_FIXED, 0.0), b.P(G_FIXED, 0.0), b.P(G_FIXED, 0.0)));
    double qw, qx, qy, qz;
    Slvs_MakeQuaternion(1, 0, 0,  0, 1, 0,  &qw, &qx, &qy, &qz);
    b.normal = b.E(Slvs_MakeNormal3d(++b.eh, G_FIXED,
                   b.P(G_FIXED, qw), b.P(G_FIXED, qx), b.P(G_FIXED, qy), b.P(G_FIXED, qz)));
    b.wp = b.E(Slvs_MakeWorkplane(++b.eh, G_FIXED, origin, b.normal));

    // Unit direction references for the axis-projected distance constraints. Both live in
    // G_FIXED, so they are held constant and add no DOF to the system.
    // libslvs defines a LINE_SEGMENT's direction as point[0] - point[1] (entity.cpp
    // VectorGetExprs), so the unit vector's head is listed first to yield +X / +Y.
    const Slvs_hEntity dir_x [[maybe_unused]] = b.E(Slvs_MakeLineSegment(++b.eh, G_FIXED, b.wp,
                                                       b.pt2d(G_FIXED, 1.0, 0.0), b.pt2d(G_FIXED, 0.0, 0.0)));
    const Slvs_hEntity dir_y [[maybe_unused]] = b.E(Slvs_MakeLineSegment(++b.eh, G_FIXED, b.wp,
                                                       b.pt2d(G_FIXED, 0.0, 1.0), b.pt2d(G_FIXED, 0.0, 0.0)));

    // ---- Entities -------------------------------------------------------------------
    std::vector<Slots> slot(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        const SketchEntity& e = entities[i];
        Slots s;
        switch (e.type) {
        case SketchEntity::Type::Line:
            s.p0   = b.pt2d(G_SK, e.p0.x(), e.p0.y());
            s.p1   = b.pt2d(G_SK, e.p1.x(), e.p1.y());
            s.prim = b.E(Slvs_MakeLineSegment(++b.eh, G_SK, b.wp, s.p0, s.p1));
            break;
        case SketchEntity::Type::Point:
            s.p0 = b.pt2d(G_SK, e.p0.x(), e.p0.y());
            break;
        case SketchEntity::Type::Circle: {
            s.center = b.pt2d(G_SK, e.center.x(), e.center.y());
            s.p0     = s.center;                          // p0 mirrors centre for circles
            s.rparam = b.P(G_SK, e.radius > 1e-9 ? e.radius : 1.0);
            Slvs_hEntity dist = b.E(Slvs_MakeDistance(++b.eh, G_SK, b.wp, s.rparam));
            s.prim   = b.E(Slvs_MakeCircle(++b.eh, G_SK, b.wp, s.center, b.normal, dist));
            break;
        }
        case SketchEntity::Type::Arc:
            s.center = b.pt2d(G_SK, e.center.x(), e.center.y());
            s.p0     = b.pt2d(G_SK, e.p0.x(), e.p0.y());   // start
            s.p1     = b.pt2d(G_SK, e.p1.x(), e.p1.y());   // end
            s.prim   = b.E(Slvs_MakeArcOfCircle(++b.eh, G_SK, b.wp, b.normal, s.center, s.p0, s.p1));
            break;
        // libslvs has no conic entity (scope note): register the ellipse's defining
        // points only (center + arc endpoints) so center/endpoint constraints solve;
        // the a/b/phi shape params pass through unsolved.
        case SketchEntity::Type::Ellipse:
            s.center = b.pt2d(G_SK, e.center.x(), e.center.y());
            s.p0     = s.center;                          // p0 mirrors centre (circle convention)
            break;
        case SketchEntity::Type::EllipseArc:
            s.center = b.pt2d(G_SK, e.center.x(), e.center.y());
            s.p0     = b.pt2d(G_SK, e.p0.x(), e.p0.y());   // start
            s.p1     = b.pt2d(G_SK, e.p1.x(), e.p1.y());   // end
            break;
        // No native slvs curve for an arbitrary-degree spline: register the control
        // poles as point2d so endpoints (and any pole-targeted constraint) solve. The
        // OCCT curve is rebuilt from the solved poles. p0/p1 mirror first/last pole so
        // Coincident at the spline ends closes loops just like a Line.
        case SketchEntity::Type::BSpline:
            s.pts.reserve(e.ctrl.size());
            for (const Vec2d& cp : e.ctrl)
                s.pts.push_back(b.pt2d(G_SK, cp.x(), cp.y()));
            if (!s.pts.empty()) { s.p0 = s.pts.front(); s.p1 = s.pts.back(); }
            break;
        }
        slot[i] = s;
    }

    auto valid = [&](int ei) { return ei >= 0 && ei < int(entities.size()); };
    auto ptOf  = [&](int ei, Role r) -> Slvs_hEntity {
        if (!valid(ei)) return 0;
        const Slots& s = slot[ei];
        switch (r) {
        case Role::P0:     return s.p0;
        case Role::P1:     return s.p1;
        case Role::Center: return s.center ? s.center : s.p0;
        }
        return 0;
    };
    auto primOf = [&](int ei) -> Slvs_hEntity { return valid(ei) ? slot[ei].prim : 0; };
    auto coordOf = [&](int ei, Role r) -> Vec2d {
        if (!valid(ei)) return Vec2d(0, 0);
        const SketchEntity& e = entities[ei];
        switch (r) { case Role::P0: return e.p0; case Role::P1: return e.p1; case Role::Center: return e.center; }
        return e.p0;
    };
    // A fixed reference point at (x,y) — used to pin coordinates (Fix / LockX / LockY).
    auto fixedRef = [&](double x, double y) -> Slvs_hEntity { return b.pt2d(G_FIXED, x, y); };

    // ---- Constraints ----------------------------------------------------------------
    for (const auto& c : constraints) {
        // Robustness: never feed libslvs a null handle. A constraint that references an
        // entity which produced no solver primitive (Point/Ellipse/EllipseArc/BSpline get
        // no `prim`) or no point for the requested role would make Slvs FindById abort the
        // whole process. Skip such a constraint instead of crashing.
        bool ref_ok = true;
        switch (c.type) {
        case CT::Coincident: case CT::Horizontal: case CT::Vertical: case CT::Distance:
            ref_ok = ptOf(c.ea, c.ra) && ptOf(c.eb, c.rb); break;
        case CT::DistanceX:
        case CT::DistanceY:
            ref_ok = ptOf(c.ea, c.ra) && ptOf(c.eb, c.rb); break;
        case CT::Concentric:
            ref_ok = ptOf(c.ea, Role::Center) && ptOf(c.eb, Role::Center); break;
        case CT::Fix: case CT::LockX: case CT::LockY:
            ref_ok = ptOf(c.ea, c.ra) != 0; break;
        case CT::EqualLength: case CT::Parallel: case CT::Perpendicular:
        case CT::Angle: case CT::Tangent:
            ref_ok = primOf(c.ea) && primOf(c.eb); break;
        case CT::Radius: case CT::Diameter:
            ref_ok = primOf(c.ea) != 0; break;
        case CT::Midpoint:
            ref_ok = ptOf(c.ea, c.ra) && primOf(c.eb); break;
        case CT::Symmetric:
            ref_ok = ptOf(c.ea, c.ra) && ptOf(c.eb, c.rb) && primOf(c.ec); break;
        case CT::PointOnLine: case CT::PointOnObject:
            ref_ok = ptOf(c.ea, c.ra) && primOf(c.eb); break;
        case CT::EqualRadius:
        case CT::Collinear:
            ref_ok = primOf(c.ea) && primOf(c.eb); break;
        }
        if (!ref_ok) continue;
        switch (c.type) {
        case CT::Coincident:
            b.C(SLVS_C_POINTS_COINCIDENT, 0, ptOf(c.ea, c.ra), ptOf(c.eb, c.rb), 0, 0);
            break;
        case CT::Concentric:
            b.C(SLVS_C_POINTS_COINCIDENT, 0, ptOf(c.ea, Role::Center), ptOf(c.eb, Role::Center), 0, 0);
            break;
        case CT::Horizontal:
            b.C(SLVS_C_HORIZONTAL, 0, ptOf(c.ea, c.ra), ptOf(c.eb, c.rb), 0, 0);
            break;
        case CT::Vertical:
            b.C(SLVS_C_VERTICAL, 0, ptOf(c.ea, c.ra), ptOf(c.eb, c.rb), 0, 0);
            break;
        case CT::Distance:
            b.C(SLVS_C_PT_PT_DISTANCE, c.value, ptOf(c.ea, c.ra), ptOf(c.eb, c.rb), 0, 0);
            break;
        case CT::DistanceX:
            // Distance between the two points measured along X only: project the vector
            // between them onto the fixed unit X direction.
            b.C(SLVS_C_PROJ_PT_DISTANCE, c.value, ptOf(c.ea, c.ra), ptOf(c.eb, c.rb), dir_x, 0);
            break;
        case CT::DistanceY:
            b.C(SLVS_C_PROJ_PT_DISTANCE, c.value, ptOf(c.ea, c.ra), ptOf(c.eb, c.rb), dir_y, 0);
            break;
        case CT::Fix: {
            const Vec2d p = coordOf(c.ea, c.ra);
            b.C(SLVS_C_POINTS_COINCIDENT, 0, ptOf(c.ea, c.ra), fixedRef(p.x(), p.y()), 0, 0);
            break;
        }
        case CT::LockX: {
            const Vec2d p = coordOf(c.ea, c.ra);
            b.C(SLVS_C_VERTICAL, 0, ptOf(c.ea, c.ra), fixedRef(c.value, p.y()), 0, 0);
            break;
        }
        case CT::LockY: {
            const Vec2d p = coordOf(c.ea, c.ra);
            b.C(SLVS_C_HORIZONTAL, 0, ptOf(c.ea, c.ra), fixedRef(p.x(), c.value), 0, 0);
            break;
        }
        case CT::EqualLength:
            b.C(SLVS_C_EQUAL_LENGTH_LINES, 0, 0, 0, primOf(c.ea), primOf(c.eb));
            break;
        case CT::Parallel:
            b.C(SLVS_C_PARALLEL, 0, 0, 0, primOf(c.ea), primOf(c.eb));
            break;
        case CT::Perpendicular:
            b.C(SLVS_C_PERPENDICULAR, 0, 0, 0, primOf(c.ea), primOf(c.eb));
            break;
        case CT::Midpoint:
            b.C(SLVS_C_AT_MIDPOINT, 0, ptOf(c.ea, c.ra), 0, primOf(c.eb), 0);
            break;
        case CT::Symmetric:
            // ptA, ptB symmetric about the axis line (ec).
            b.C(SLVS_C_SYMMETRIC_LINE, 0, ptOf(c.ea, c.ra), ptOf(c.eb, c.rb), primOf(c.ec), 0);
            break;
        case CT::Angle:
            // model stores radians; slvs angle is in degrees.
            b.C(SLVS_C_ANGLE, c.value * 180.0 / M_PI, 0, 0, primOf(c.ea), primOf(c.eb));
            break;
        case CT::Radius:
            b.C(SLVS_C_DIAMETER, 2.0 * c.value, 0, 0, primOf(c.ea), 0);
            break;
        case CT::Diameter:
            b.C(SLVS_C_DIAMETER, c.value, 0, 0, primOf(c.ea), 0);
            break;
        case CT::Tangent: {
            const bool aCurve = valid(c.ea) && entities[c.ea].type != SketchEntity::Type::Line;
            const bool bCurve = valid(c.eb) && entities[c.eb].type != SketchEntity::Type::Line;
            if (aCurve && bCurve)
                b.C(SLVS_C_CURVE_CURVE_TANGENT, 0, 0, 0, primOf(c.ea), primOf(c.eb));
            else {
                const int ci = aCurve ? c.ea : c.eb;   // the curve
                const int li = aCurve ? c.eb : c.ea;   // the line
                if (valid(ci) && entities[ci].type == SketchEntity::Type::Circle) {
                    // A FULL circle cannot use SLVS_C_ARC_LINE_TANGENT. That constraint reads
                    // arc->point[1] / point[2] — the arc's endpoints (see constrainteq.cpp,
                    // Type::ARC_LINE_TANGENT) — and a circle entity only has point[0], its
                    // centre. The zero handles send FindById into "Cannot find handle", which
                    // ABORTS the process rather than failing the solve, taking every later test
                    // with it. It is also the wrong equation for a circle: it only makes the
                    // line perpendicular to the radius AT AN ENDPOINT that does not exist.
                    //
                    // For a circle, tangency is exactly "the centre sits one radius away from
                    // the line", which slvs expresses directly.
                    //
                    // ponytail: the radius is captured here rather than tied as a variable —
                    // the C API takes a constant distance and offers no way to reference the
                    // circle's radius parameter. Exact whenever the radius is fixed or simply
                    // not being changed by another constraint in the same solve; if some other
                    // constraint drives the radius, re-solving restores tangency. Tying them
                    // would need an auxiliary point constrained onto both circle and line.
                    b.C(SLVS_C_PT_LINE_DISTANCE, entities[ci].radius,
                        ptOf(ci, Role::Center), 0, primOf(li), 0);
                } else {
                    b.C(SLVS_C_ARC_LINE_TANGENT, 0, 0, 0, primOf(ci), primOf(li));
                }
            }
            break;
        }
        case CT::PointOnLine:
            if (std::abs(c.value) < 1e-9)
                b.C(SLVS_C_PT_ON_LINE, 0, ptOf(c.ea, c.ra), 0, primOf(c.eb), 0);
            else
                b.C(SLVS_C_PT_LINE_DISTANCE, std::abs(c.value), ptOf(c.ea, c.ra), 0, primOf(c.eb), 0);
            break;
        case CT::PointOnObject:
            // Point (ea,ra) lies on entity edge eb: a circle rim -> PT_ON_CIRCLE,
            // otherwise the segment line -> PT_ON_LINE.
            if (valid(c.eb) && entities[c.eb].type == SketchEntity::Type::Circle)
                b.C(SLVS_C_PT_ON_CIRCLE, 0, ptOf(c.ea, c.ra), 0, primOf(c.eb), 0);
            else
                b.C(SLVS_C_PT_ON_LINE, 0, ptOf(c.ea, c.ra), 0, primOf(c.eb), 0);
            break;
        case CT::EqualRadius:
            b.C(SLVS_C_EQUAL_RADIUS, 0, 0, 0, primOf(c.ea), primOf(c.eb));
            break;
        case CT::Collinear:
            // libslvs has no collinear code. Two lines are collinear iff they are parallel
            // AND a point of one lies on the other's infinite line — emit both.
            b.C(SLVS_C_PARALLEL, 0, 0, 0, primOf(c.ea), primOf(c.eb));
            // Point-on-infinite-line via PT_LINE_DISTANCE=0 rather than PT_ON_LINE: the
            // latter creates an internal `valP` param that this port's Slvs_Solve leaves at
            // 0 in the working set (ModifyToSatisfy only updates SK.param), so an already
            // collinear pair drifts. PT_LINE_DISTANCE=0 is the same condition with no extra
            // parameter, so an already-satisfied solve is a clean no-op.
            b.C(SLVS_C_PT_LINE_DISTANCE, 0, ptOf(c.eb, Role::P0), 0, primOf(c.ea), 0);
            break;
        }
    }

    // ---- Solve ----------------------------------------------------------------------
    Slvs_System sys;
    std::memset(&sys, 0, sizeof(sys));
    sys.param       = b.params.data();      sys.params      = int(b.params.size());
    sys.entity      = b.ents.data();        sys.entities    = int(b.ents.size());
    sys.constraint  = b.cons.data();        sys.constraints = int(b.cons.size());
    std::vector<Slvs_hConstraint> failed(b.cons.size() + 1, 0);
    sys.failed = failed.data();
    sys.faileds = int(failed.size());
    sys.calculateFaileds = 1;

    // Drag pin: feed the dragged point's two params into sys.dragged[] so the solver
    // favours keeping that point at the cursor and re-solves the rest around it.
    if (dragged_ei >= 0) {
        const Slvs_hEntity h = ptOf(dragged_ei, dragged_role);
        for (const Slvs_Entity& en : b.ents)
            if (en.h == h) { sys.dragged[0] = en.param[0]; sys.dragged[1] = en.param[1]; break; }
    }

    Slvs_Solve(&sys, G_SK);

    out.result = sys.result;
    out.dof    = sys.dof;
    out.ok     = (sys.result == SLVS_RESULT_OKAY);

    // Map solved param handles -> values, then read points back.
    std::unordered_map<Slvs_hParam, double> pv;
    pv.reserve(sys.params * 2);
    for (int i = 0; i < sys.params; ++i) pv[sys.param[i].h] = sys.param[i].val;
    std::unordered_map<Slvs_hEntity, const Slvs_Entity*> byH;
    byH.reserve(sys.entities * 2);
    for (int i = 0; i < sys.entities; ++i) byH[sys.entity[i].h] = &sys.entity[i];
    auto coord = [&](Slvs_hEntity h) -> Vec2d {
        auto it = byH.find(h);
        if (it == byH.end()) return Vec2d(0, 0);
        return Vec2d(pv[it->second->param[0]], pv[it->second->param[1]]);
    };

    // Map failed constraint handles back to indices into `constraints`.
    if (!out.ok && sys.faileds > 0) {
        std::unordered_map<Slvs_hConstraint, int> chToIdx;
        // constraint handles were assigned in order starting after the fixed group; the
        // i-th sketch constraint in b.cons has handle = its position. Rebuild by scanning.
        for (size_t k = 0; k < b.cons.size(); ++k) chToIdx[b.cons[k].h] = int(k);
        for (int i = 0; i < sys.faileds; ++i) {
            auto it = chToIdx.find(failed[i]);
            if (it != chToIdx.end() && it->second < int(constraints.size()))
                out.bad.push_back(it->second);
        }
    }

    // ---- Read solved geometry back --------------------------------------------------
    // ONLY on success. A failed solve leaves libslvs' params holding its last Newton
    // iterate — geometry that satisfies nothing and is usually wildly deformed. Writing
    // that back made every rejected attempt destructive: the caller rolls the constraints
    // back, but the sketch it rolls back to is already wreckage, so the next attempt starts
    // from the corpse. The fillet degrade ladder hit this on every corner — rung 1 (a
    // tangent on each leg) is legitimately over-constrained against the legs' own H/V, and
    // its wreckage then failed rungs 2 and 3, which solve cleanly on their own. The arc
    // ended up with no constraints at all and the solver snapped the corner shut. snaporca-pl5.
    if (!out.ok) return out;
    for (size_t i = 0; i < entities.size(); ++i) {
        SketchEntity& e = entities[i];
        const Slots&  s = slot[i];
        if (s.p0)     e.p0     = coord(s.p0);
        if (s.p1)     e.p1     = coord(s.p1);
        if (s.center) e.center = coord(s.center);

        if (e.type == SketchEntity::Type::BSpline) {
            for (size_t k = 0; k < s.pts.size() && k < e.ctrl.size(); ++k)
                e.ctrl[k] = coord(s.pts[k]);
            if (!e.ctrl.empty()) { e.p0 = e.ctrl.front(); e.p1 = e.ctrl.back(); }
        } else if (e.type == SketchEntity::Type::Circle) {
            if (s.rparam) { auto it = pv.find(s.rparam); if (it != pv.end()) e.radius = it->second; }
            e.p0 = e.center;
        } else if (e.type == SketchEntity::Type::Arc && s.center) {
            // Reflow arc angles from solved centre + endpoints, preserving sweep sign.
            const double old_sweep = e.end_angle - e.start_angle;
            const double ns = std::atan2(e.p0.y() - e.center.y(), e.p0.x() - e.center.x());
            const double ne = std::atan2(e.p1.y() - e.center.y(), e.p1.x() - e.center.x());
            double sweep = ne - ns;
            const double TWO_PI = 2.0 * M_PI;
            while (sweep <= -TWO_PI) sweep += TWO_PI;
            while (sweep >=  TWO_PI) sweep -= TWO_PI;
            if (old_sweep >= 0.0 && sweep < 0.0) sweep += TWO_PI;
            if (old_sweep <  0.0 && sweep > 0.0) sweep -= TWO_PI;
            e.start_angle = ns;
            e.end_angle   = ns + sweep;
            e.radius = 0.5 * ((e.p0 - e.center).norm() + (e.p1 - e.center).norm());
        }
    }

    return out;
}

// libslvs carries a COMPILE-TIME ceiling: solvespace.h declares `enum { MAX_UNKNOWNS = 1024 }`
// and sizes the System's param and equation arrays with it. solve_system() hands the solver every
// entity in the sketch, constrained or not, at 2 params per point — so a sketch of about 480 lines
// is the last one that fits, and the very next one comes back TOO_MANY_UNKNOWNS.
//
// What that did, before this: DesignSketchTool::try_add_constraints rolls the whole batch back
// when the solve fails, so the auto-constraint pass over a large sketch dropped EVERY constraint
// it had just inferred. Measured on the rig — 480 lines: 960 constraints, dof 480. 520 lines:
// 0 constraints, dof unknown. Nothing was said, and from there on no dimension and no constraint
// could ever be applied to that sketch, because each attempt re-solved the same oversized system
// and was rejected in turn. A typed length simply did nothing.
//
// Constraints only couple entities that SHARE a point, so a sketch is naturally a set of
// independent systems — a plate with 300 cut-outs is 301 little problems, not one big one.
// Solving them separately keeps every one of them far under the ceiling AND is faster, since the
// solver's work is superlinear in system size.
//
// The whole system is still tried FIRST, and this runs only on TOO_MANY_UNKNOWNS, so every sketch
// that fits today keeps its exact current behaviour, including its reported degrees of freedom.
// A genuinely over-constrained sketch still fails: the conflict lives inside one component and
// that component still rejects it.
static SketchSolveResult solve_partitioned(std::vector<SketchEntity>& entities,
                                           const std::vector<SketchEntityConstraintDef>& constraints,
                                           int dragged_ei, Role dragged_role)
{
    const int n = int(entities.size());
    std::vector<int> parent(n);
    for (int i = 0; i < n; ++i) parent[i] = i;
    std::function<int(int)> find = [&](int a) {
        while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; }
        return a;
    };
    auto unite = [&](int a, int b) {
        if (a < 0 || b < 0 || a >= n || b >= n) return;
        a = find(a); b = find(b);
        if (a != b) parent[a] = b;
    };
    for (const auto& c : constraints) { unite(c.ea, c.eb); unite(c.ea, c.ec); }

    // Group the constraints by the component they belong to.
    std::map<int, std::vector<int>> groups;
    for (size_t i = 0; i < constraints.size(); ++i) {
        const int a = constraints[i].ea;
        if (a < 0 || a >= n) continue;
        groups[find(a)].push_back(int(i));
    }

    SketchSolveResult out;
    out.ok  = true;
    out.dof = 0;
    // Solve into COPIES and commit only if every component succeeded. The contract callers rely
    // on is all-or-nothing — try_add_constraints rolls the batch back and expects the geometry it
    // rolls back to be untouched — and partial writes would break it.
    std::vector<std::pair<std::vector<int>, std::vector<SketchEntity>>> solved;
    for (const auto& [root, cidx] : groups) {
        std::vector<int> ents;                       // global indices, in order
        std::map<int, int> local;                    // global -> local
        auto take = [&](int e) {
            if (e < 0 || e >= n || local.count(e)) return;
            local[e] = int(ents.size());
            ents.push_back(e);
        };
        for (int ci : cidx) { take(constraints[ci].ea); take(constraints[ci].eb); take(constraints[ci].ec); }
        std::vector<SketchEntity> sub;
        sub.reserve(ents.size());
        for (int e : ents) sub.push_back(entities[e]);
        std::vector<SketchEntityConstraintDef> subc;
        subc.reserve(cidx.size());
        for (int ci : cidx) {
            SketchEntityConstraintDef d = constraints[ci];
            auto map1 = [&](int& e) { e = (e >= 0 && local.count(e)) ? local[e] : -1; };
            map1(d.ea); map1(d.eb); map1(d.ec);
            subc.push_back(d);
        }
        const int sub_drag = (dragged_ei >= 0 && local.count(dragged_ei)) ? local[dragged_ei] : -1;
        SketchSolveResult r = solve_system(sub, subc, sub_drag, dragged_role);
        if (!r.ok) {
            out.ok     = false;
            out.result = r.result;
            for (int bi : r.bad)
                if (bi >= 0 && bi < int(cidx.size())) out.bad.push_back(cidx[bi]);
        }
        if (r.dof > 0) out.dof += r.dof;
        solved.emplace_back(std::move(ents), std::move(sub));
    }
    if (!out.ok) return out;
    for (auto& [ents, sub] : solved)
        for (size_t k = 0; k < ents.size(); ++k) entities[ents[k]] = sub[k];
    return out;
}

static SketchSolveResult solve_impl(std::vector<SketchEntity>& entities,
                                    const std::vector<SketchEntityConstraintDef>& constraints,
                                    int dragged_ei, Role dragged_role)
{
    SketchSolveResult out = solve_system(entities, constraints, dragged_ei, dragged_role);
    if (out.ok || out.result != SLVS_RESULT_TOO_MANY_UNKNOWNS) return out;
    return solve_partitioned(entities, constraints, dragged_ei, dragged_role);
}

SketchSolveResult sketch_solve(std::vector<SketchEntity>& entities,
                               const std::vector<SketchEntityConstraintDef>& constraints)
{
    return solve_impl(entities, constraints, -1, Role::P0);
}

SketchSolveResult sketch_solve_drag(std::vector<SketchEntity>& entities,
                                    const std::vector<SketchEntityConstraintDef>& constraints,
                                    int dragged_ei, SketchPointRole dragged_role)
{
    return solve_impl(entities, constraints, dragged_ei, dragged_role);
}

} // namespace Slic3r
