#include "MultiplanarBaseC.hpp"

#include "../PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace MultiplanarBaseC {

bool is_vertical(const Vec3d &tool_axis)
{
    const double len = tool_axis.norm();
    if (len <= 0.)
        return false;
    const Vec3d n = tool_axis / len;
    return std::hypot(n.x(), n.y()) < 1e-6 && n.z() > 0.;
}

Choice choose(const std::vector<std::vector<Member>> &layers)
{
    Choice out;
    bool   base_seen = false;
    for (size_t l = 0; l < layers.size(); ++ l) {
        const std::vector<Member> &members = layers[l];
        for (size_t m = 0; m < members.size(); ++ m) {
            if (is_vertical(members[m].tool_axis)) {
                base_seen = true;
                continue;
            }
            // The first angled member in print order.
            out.layer  = int(l);
            out.member = int(m);
            if (! members[m].is_segment) {
                out.reason = "the first angled member of the group is not a cut-plane segment";
                return out;
            }
            if (! base_seen) {
                out.reason = "nothing prints before the first angled chunk";
                return out;
            }
            double b = 0., c = 0.;
            tool_axis_to_bc(members[m].tool_axis, b, c);
            for (size_t k = m + 1; k < members.size(); ++ k)
                if (! is_vertical(members[k].tool_axis)) {
                    double bk = 0., ck = 0.;
                    tool_axis_to_bc(members[k].tool_axis, bk, ck);
                    if (std::abs(std::remainder(ck - c, 2. * PI)) > 1e-6)
                        out.ambiguous = true;
                }
            out.c_rad = c;
            return out;
        }
    }
    out.reason = "the group has no angled chunk";
    return out;
}

Vec2d bed_to_machine_xy(const Vec2d &bed, const Vec2d &pivot, double c_rad)
{
    const double cs = std::cos(c_rad), sn = std::sin(c_rad);
    const Vec2d  r  = bed - pivot;
    // Rz(-C)
    return pivot + Vec2d(cs * r.x() + sn * r.y(), - sn * r.x() + cs * r.y());
}

Vec2d machine_to_bed_xy(const Vec2d &machine, const Vec2d &pivot, double c_rad)
{
    const double cs = std::cos(c_rad), sn = std::sin(c_rad);
    const Vec2d  r  = machine - pivot;
    // Rz(C)
    return pivot + Vec2d(cs * r.x() - sn * r.y(), sn * r.x() + cs * r.y());
}

namespace {

// Even-odd point in polygon; the polygon is closed implicitly.
bool inside_polygon(const std::vector<Vec2d> &poly, const Vec2d &p)
{
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i ++) {
        const Vec2d &a = poly[i];
        const Vec2d &b = poly[j];
        if ((a.y() > p.y()) != (b.y() > p.y())) {
            const double x = a.x() + (p.y() - a.y()) * (b.x() - a.x()) / (b.y() - a.y());
            if (p.x() < x)
                inside = ! inside;
        }
    }
    return inside;
}

double distance_to_boundary(const std::vector<Vec2d> &poly, const Vec2d &p)
{
    double best = std::numeric_limits<double>::max();
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i ++) {
        const Vec2d  a  = poly[j];
        const Vec2d  ab = poly[i] - a;
        const double l2 = ab.squaredNorm();
        const double t  = l2 > 0. ? std::clamp((p - a).dot(ab) / l2, 0., 1.) : 0.;
        best = std::min(best, (a + t * ab - p).norm());
    }
    return best;
}

} // namespace

bool fits_at_c(const std::vector<Vec2d> &footprint, const std::vector<Vec2d> &area, const Vec2d &pivot,
               double c_rad, double tolerance, Vec2d *worst)
{
    if (area.size() < 3)
        return true;   // no printable area to hold it to
    bool   fits      = true;
    double worst_out = -1.;
    for (const Vec2d &bed : footprint) {
        const Vec2d m = bed_to_machine_xy(bed, pivot, c_rad);
        if (inside_polygon(area, m))
            continue;
        const double out = distance_to_boundary(area, m);
        if (out <= tolerance)
            continue;
        fits = false;
        if (out > worst_out) {
            worst_out = out;
            if (worst != nullptr)
                *worst = m;
        }
    }
    return fits;
}

} // namespace MultiplanarBaseC
} // namespace Slic3r
