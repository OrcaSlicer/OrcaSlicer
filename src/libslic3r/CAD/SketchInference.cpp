#include "libslic3r/CAD/SketchInference.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

// Candidate target collected during the scan; we keep the closest within each
// priority tier and resolve ties by tier then distance.
namespace {
struct Cand {
    InferenceSnap::Kind kind{InferenceSnap::Kind::None};
    int                 entity{-1};
    SketchPointRole     role{SketchPointRole::P0};
    Vec2d               point{0, 0};
    double              dist{0.0};
};

// Lower number = higher priority.
int tier(InferenceSnap::Kind k)
{
    switch (k) {
    case InferenceSnap::Kind::Endpoint: return 0;
    case InferenceSnap::Kind::Center:   return 1;
    case InferenceSnap::Kind::Origin:   return 2;
    case InferenceSnap::Kind::Midpoint: return 3;
    case InferenceSnap::Kind::OnEdge:   return 4;
    default:                            return 9;
    }
}
} // namespace

InferenceSnap infer_point_snap(const std::vector<SketchEntity>& entities,
                               const Vec2d& query, double tol,
                               bool include_origin)
{
    Cand best;
    best.kind = InferenceSnap::Kind::None;
    best.point = query;

    auto offer = [&](InferenceSnap::Kind k, int ent, SketchPointRole r, const Vec2d& q) {
        const double d = (q - query).norm();
        if (d > tol) return;
        const bool better = (best.kind == InferenceSnap::Kind::None) ||
                            (tier(k) < tier(best.kind)) ||
                            (tier(k) == tier(best.kind) && d < best.dist);
        if (better) { best.kind = k; best.entity = ent; best.role = r; best.point = q; best.dist = d; }
    };

    for (size_t i = 0; i < entities.size(); ++i) {
        const SketchEntity& e = entities[i];
        const int ei = int(i);
        switch (e.type) {
        case SketchEntity::Type::Line: {
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P0, e.p0);
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P1, e.p1);
            offer(InferenceSnap::Kind::Midpoint, ei, SketchPointRole::P0, 0.5 * (e.p0 + e.p1));
            // Projection onto the segment interior (PointOnObject candidate).
            const Vec2d d = e.p1 - e.p0;
            const double L2 = d.squaredNorm();
            if (L2 > 1e-12) {
                double t = (query - e.p0).dot(d) / L2;
                if (t > 0.02 && t < 0.98)
                    offer(InferenceSnap::Kind::OnEdge, ei, SketchPointRole::P0, e.p0 + t * d);
            }
            break;
        }
        case SketchEntity::Type::Arc: {
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P0, e.p0);
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P1, e.p1);
            offer(InferenceSnap::Kind::Center,   ei, SketchPointRole::Center, e.center);
            // Mid-arc point, so an arc is as snappable in its middle as a line is.
            const double am = 0.5 * (e.start_angle + e.end_angle);
            offer(InferenceSnap::Kind::Midpoint, ei, SketchPointRole::P0,
                  Vec2d(e.center.x() + e.radius * std::cos(am),
                        e.center.y() + e.radius * std::sin(am)));
            break;
        }
        case SketchEntity::Type::Circle: {
            offer(InferenceSnap::Kind::Center, ei, SketchPointRole::Center, e.center);
            // Nearest point on the circle rim (PointOnObject candidate).
            const Vec2d v = query - e.center;
            const double n = v.norm();
            if (n > 1e-9 && e.radius > 1e-9)
                offer(InferenceSnap::Kind::OnEdge, ei, SketchPointRole::Center,
                      e.center + v * (e.radius / n));
            break;
        }
        case SketchEntity::Type::Point:
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P0, e.p0);
            break;
        case SketchEntity::Type::EllipseArc:
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P0, e.p0);
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P1, e.p1);
            offer(InferenceSnap::Kind::Center,   ei, SketchPointRole::Center, e.center);
            break;
        case SketchEntity::Type::Ellipse:
            offer(InferenceSnap::Kind::Center, ei, SketchPointRole::Center, e.center);
            break;
        case SketchEntity::Type::BSpline:
            // Endpoints (first/last pole) snap for loop closure.
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P0, e.p0);
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P1, e.p1);
            break;
        }
    }

    if (include_origin)
        offer(InferenceSnap::Kind::Origin, -1, SketchPointRole::P0, Vec2d(0, 0));

    InferenceSnap r;
    r.kind = best.kind; r.entity = best.entity; r.role = best.role; r.point = best.point;
    return r;
}

std::optional<SketchConstraintType>
infer_axis_constraint(const Vec2d& anchor, const Vec2d& tip, double ang_tol_rad)
{
    const Vec2d d = tip - anchor;
    if (d.squaredNorm() < 1e-12) return std::nullopt;
    const double ang = std::atan2(std::abs(d.y()), std::abs(d.x())); // 0=horizontal, pi/2=vertical
    if (ang <= ang_tol_rad)                 return SketchConstraintType::Horizontal;
    if (ang >= M_PI / 2.0 - ang_tol_rad)    return SketchConstraintType::Vertical;
    return std::nullopt;
}

// Unsigned angle between two (unnormalized) direction vectors, in [0, pi]. 0 = same
// direction, pi = opposite, pi/2 = perpendicular. Inputs must be non-degenerate.
// static: this is a file-local helper, not part of the module's interface -- at namespace
// scope with external linkage it would be a link-time collision waiting to happen.
static double unsigned_angle(const Vec2d& a, const Vec2d& b)
{
    const double cross = a.x() * b.y() - a.y() * b.x();
    const double dot   = a.x() * b.x() + a.y() * b.y();
    return std::atan2(std::abs(cross), dot);
}

std::vector<SketchEntityConstraintDef>
infer_relations(const std::vector<SketchEntity>& entities, int new_ei,
                double ang_tol_rad, double len_tol_frac)
{
    std::vector<SketchEntityConstraintDef> out;
    if (new_ei <= 0 || new_ei >= int(entities.size())) return out;

    // AT MOST ONE constraint per rule per new entity, not one per PAIR. Without this the
    // function is quadratic in the sketch: a drawing with 200 equal holes yields ~20000
    // EqualRadius candidates, the batch is rejected as over-constrained, and the caller's
    // one-at-a-time fallback then runs a solve per constraint. Measured 2026-08-31: that
    // pinned the app at 95% of a core with the MCP socket unresponsive -- the same failure
    // the axes batch above already carries a warning about. Keep the best candidate only.
    int    best_ang_j = -1, best_rad_j = -1, best_tan_j = -1;
    double best_ang_err = 1e30, best_rad_err = 1e30, best_tan_err = 1e30;
    SketchConstraintType best_ang_type = SketchConstraintType::Parallel;

    const SketchEntity& n = entities[new_ei];
    const bool n_line  = n.type == SketchEntity::Type::Line;
    const bool n_curve = n.type == SketchEntity::Type::Arc || n.type == SketchEntity::Type::Circle;
    if (!n_line && !n_curve) return out;                       // not a Line / Arc / Circle
    if (n_line  && (n.p1 - n.p0).squaredNorm() < 1e-18) return out; // degenerate
    if (n_curve && n.radius < 1e-9)                      return out;

    for (int j = 0; j < new_ei; ++j) {
        const SketchEntity& o = entities[j];
        const bool o_line  = o.type == SketchEntity::Type::Line;
        const bool o_curve = o.type == SketchEntity::Type::Arc || o.type == SketchEntity::Type::Circle;
        if (!o_line && !o_curve) continue;
        if (o_line  && (o.p1 - o.p0).squaredNorm() < 1e-18) continue;
        if (o_curve && o.radius < 1e-9)                      continue;

        if (n_line && o_line) {
            // R1 — parallel / perpendicular, restricted to CONNECTED lines. Connection is
            // what keeps this from firing on every distant line that is roughly parallel.
            const bool connected = (n.p0 - o.p0).squaredNorm() <= 1e-14 ||
                                   (n.p0 - o.p1).squaredNorm() <= 1e-14 ||
                                   (n.p1 - o.p0).squaredNorm() <= 1e-14 ||
                                   (n.p1 - o.p1).squaredNorm() <= 1e-14;
            if (!connected) continue;
            const double ang = unsigned_angle(n.p1 - n.p0, o.p1 - o.p0);
            const double par_err = std::min(ang, M_PI - ang);
            const double per_err = std::abs(ang - M_PI / 2.0);
            if (par_err <= ang_tol_rad && par_err < best_ang_err) {
                best_ang_err = par_err; best_ang_j = j;
                best_ang_type = SketchConstraintType::Parallel;
            } else if (per_err <= ang_tol_rad && per_err < best_ang_err) {
                best_ang_err = per_err; best_ang_j = j;
                best_ang_type = SketchConstraintType::Perpendicular;
            }
        } else if (n_curve && o_curve) {
            // R2 — equal radius between circles / arcs, relative to the larger.
            const double larger = n.radius > o.radius ? n.radius : o.radius;
            const double err = std::abs(n.radius - o.radius) / larger;
            if (err <= len_tol_frac && err < best_rad_err) { best_rad_err = err; best_rad_j = j; }
        } else {
            // R3 — tangent where a line meets a circle / arc at a shared endpoint, and only
            // when the line is ALREADY perpendicular to the radius at that point.
            const SketchEntity& ln = n_line ? n : o;
            const SketchEntity& cv = n_line ? o : n;
            const Vec2d ldir = ln.p1 - ln.p0;
            bool tangent = false;
            const Vec2d le[2] = { ln.p0, ln.p1 };
            for (int k = 0; k < 2 && !tangent; ++k) {
                if (cv.type == SketchEntity::Type::Arc) {
                    const Vec2d ce[2] = { cv.p0, cv.p1 };
                    for (int m = 0; m < 2; ++m) {
                        if ((le[k] - ce[m]).squaredNorm() > 1e-14) continue;
                        const Vec2d r = ce[m] - cv.center;
                        if (r.squaredNorm() < 1e-18) continue;
                        tangent = std::abs(unsigned_angle(ldir, r) - M_PI / 2.0) <= ang_tol_rad;
                        if (tangent) break;
                    }
                } else { // Circle: shared point is a line endpoint on the rim.
                    const Vec2d r = le[k] - cv.center;
                    if (std::abs(r.norm() - cv.radius) > 1e-7) continue;
                    if (r.squaredNorm() < 1e-18) continue;
                    tangent = std::abs(unsigned_angle(ldir, r) - M_PI / 2.0) <= ang_tol_rad;
                }
            }
            if (tangent && best_tan_err > 0.0) { best_tan_err = 0.0; best_tan_j = j; }
        }
    }

    auto emit = [&](SketchConstraintType t, int j) {
        if (j < 0) return;
        SketchEntityConstraintDef c;
        c.type = t; c.ea = j; c.eb = new_ei;
        out.push_back(c);
    };
    emit(best_ang_type, best_ang_j);                       // R1
    emit(SketchConstraintType::EqualRadius, best_rad_j);   // R2
    emit(SketchConstraintType::Tangent,     best_tan_j);   // R3
    return out;
}

} // namespace Slic3r
