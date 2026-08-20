#include "libslic3r/CAD/SketchInference.hpp"

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
        case SketchEntity::Type::Arc:
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P0, e.p0);
            offer(InferenceSnap::Kind::Endpoint, ei, SketchPointRole::P1, e.p1);
            offer(InferenceSnap::Kind::Center,   ei, SketchPointRole::Center, e.center);
            break;
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

} // namespace Slic3r
