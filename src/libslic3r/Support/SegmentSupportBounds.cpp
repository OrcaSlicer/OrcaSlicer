#include "SegmentSupportBounds.hpp"
#include "../ClipperUtils.hpp"
#include "../Print.hpp"

#include <cmath>
#include <limits>

namespace Slic3r {

// Norms below this are numerical noise, not a direction. Deliberately far tighter than EPSILON:
// these are unit-ish normals, and a clip only ever degenerates through an authored plane that was
// already rejected upstream.
static constexpr double NORMAL_EPSILON = 1e-12;

SegmentSupportBounds::SegmentSupportBounds(const CutSegmentDescriptor &segment,
                                          const Transform3d          &object_to_slice,
                                          double                      bed_clearance,
                                          bool                        root_on_plate)
{
    // Normals are covectors: they carry through the inverse transpose of the linear part, which
    // stays correct under rotation, non-uniform scale and mirroring. Points carry through the
    // transform itself.
    const Eigen::Matrix3d normal_to_slice = object_to_slice.linear().inverse().transpose();
    for (const CutSegmentClip &clip : segment.clips) {
        const double norm_local_sq = clip.normal.squaredNorm();
        if (norm_local_sq < NORMAL_EPSILON)
            continue;
        // Clip membership is  clip.normal . p - clip.offset <= 0; negating gives the into-segment
        // form this class stores,  n . x >= d.
        Vec3d        n  = normal_to_slice * (- clip.normal);
        const double nn = n.norm();
        if (nn < NORMAL_EPSILON)
            continue;
        n /= nn;
        // Foot of the perpendicular from the object-local origin onto the clip plane -- any point
        // on the plane will do, and this one needs no basis.
        const Vec3d p = object_to_slice * (clip.normal * (clip.offset / norm_local_sq));
        // A Bed clip is the plate under a tilted segment that reaches it: a floor like the root's
        // plate below, raised by the same clearance.
        m_planes.push_back({ n, n.dot(p) + (clip.role == CutClipRole::Bed ? bed_clearance : 0.),
                             clip.role == CutClipRole::Ceiling });
    }

    if (segment.is_root() && root_on_plate) {
        // The root is the only segment that rests on the build plate; every other one rests on its
        // parent through its base plane. The root slices along +Z exactly (see
        // PrintObject::effective_layer_rotation_bc), so its slice frame differs from the instance
        // frame by translation alone: XY centering, and -build_z_min in Z, which is what puts the
        // root's own minimum at slice z = 0 -- except that a SUNK root is deliberately NOT rebased,
        // so the material under the plate is never sliced (see CutSegmentDescriptor::slice_z_base).
        // A world point's slice z is therefore world_z - slice_z_base(), so the plate (world z = 0)
        // sits at slice z = -slice_z_base(), raised
        // by bed_clearance. An object floating above the plate keeps the plate where it is (a
        // negative slice z) instead of pretending the floor is at the model's lowest material.
        m_planes.push_back({ Vec3d::UnitZ(), bed_clearance - segment.slice_z_base(), false });
    }

    m_active = ! m_planes.empty();
}

SegmentSupportBounds::SegmentSupportBounds(const PrintObject &object, double bed_clearance)
{
    // An ordinary object carries no descriptor: stay inactive so every caller keeps stock behavior.
    if (! object.is_segment()) {
        // BELT_MOD -- except on a belt printer, where the object is sliced rotated by the belt tilt
        // and the build surface it rests on is therefore a TILTED plane in its slice frame, exactly
        // like a non-root segment's cut face. One floor: the belt, raised by bed_clearance. The
        // plane is the same one the slicer clips layers with (PrintObject::bed_plane_in_slice_frame),
        // so support can never be grown to a floor the geometry does not agree with. n_bed is a
        // row of a rotation, hence unit length, so the clearance is a plain offset along it.
        // UNLAYERED_MOD -- the same for a whole-object layer rotation that tilts the plate in the
        // slice frame (support_floor_is_tilted() covers both): its support is un-rotated with the
        // part in the G-code, so the plate, not the slice-frame z = 0, is what must stop it. The
        // plane is carried into the sheared support-direction frame exactly as the segment branch
        // below does (the identity unless a lean is active).
        if (object.support_floor_is_tilted()) {
            Vec3d  n_bed;
            double k_bed;
            object.bed_plane_in_slice_frame(n_bed, k_bed);
            const Transform3d lean = object.support_lean().shear();
            const Vec3d       n_s  = lean.linear().inverse().transpose() * n_bed;
            m_planes.push_back({ n_s, (bed_clearance - k_bed) + n_s.dot(lean.translation()), false });
            m_active = true;
        }
        return;
    }
    const bool belt = object.print()->config().belt_printer.value;
    // While support is generated in a sheared frame (support direction, see SupportDirection.hpp)
    // the bounds must be expressed in that frame too: the shear is an affine map, which the
    // descriptor constructor already handles exactly. The identity when the lean is inactive.
    const Transform3d lean = object.support_lean().shear();
    *this = SegmentSupportBounds(*object.segment(), lean * object.object_to_slice(), bed_clearance, /*root_on_plate=*/! belt);
    if (belt) {
        // BELT_MOD: the root of a multi-planar belt print rests on the belt, tilted in its frame. A
        // non-root segment gets the belt too: its own floor is its base plane, and where that plane
        // runs on below the belt nothing else stopped its support -- a segment tilted off the gantry
        // grew support columns down to 7 mm under the belt. The belt is the surface all support must
        // end on, whichever segment. The plane is derived unsheared (n_bed is unit length, so the
        // clearance is a plain offset), then carried into the sheared frame like every other bound:
        // n.x >= d under x' = L x + t becomes (L^-T n).x' >= d + (L^-T n).t.
        Vec3d  n_bed;
        double k_bed;
        object.bed_plane_in_slice_frame(n_bed, k_bed);
        const Vec3d  n_s = lean.linear().inverse().transpose() * n_bed;
        const double d_s = (bed_clearance - k_bed) + n_s.dot(lean.translation());
        m_planes.push_back({ n_s, d_s, false });
        m_active = true;
    }
}

Polygons SegmentSupportBounds::valid_region_polygon(coordf_t print_z, const Polygons &column, double overlap) const
{
    Polygons valid = this->valid_region_polygon(print_z);
    if (overlap <= EPSILON || column.empty())
        return valid;
    // Past the ceilings by `overlap`: evaluate the bounds as if the layer sat that much lower. That
    // also lowers the floor test, which is why the result is only ever ADDED, within the column,
    // to the plain region.
    Polygons reach = intersection(this->valid_region_polygon(print_z - overlap), column);
    return reach.empty() ? valid : union_(valid, reach);
}

Polygons SegmentSupportBounds::on_plane_inside(const Polygons &xy, double a, double b, double d) const
{
    Polygons out = xy;
    for (const Plane &pl : m_planes) {
        if (out.empty())
            break;
        // n . (x, y, a x + b y + d) >= dd  <=>  (n.x + n.z a) x + (n.y + n.z b) y >= dd - n.z d
        const Vec3d  n2(pl.n.x() + pl.n.z() * a, pl.n.y() + pl.n.z() * b, 0.);
        const double d2 = pl.d - pl.n.z() * d;
        if (n2.head<2>().norm() < 1e-9) {
            // The bound is parallel to the plane: the whole plane is on one side of it.
            if (d2 > EPSILON)
                out.clear();
            continue;
        }
        out = intersection(out, half_plane_at(n2, d2, 0., true));
    }
    return out;
}

Polygons SegmentSupportBounds::valid_region_polygon(coordf_t print_z) const
{
    // Support is allowed inside EVERY bound -> intersection of the per-plane segment-side
    // half-planes. Ceilings participate: they clip, they just never bear load.
    Polygons result;
    bool     first = true;
    for (const Plane &pl : m_planes) {
        Polygons hp = half_plane(pl, print_z, true);
        if (first) {
            result = std::move(hp);
            first  = false;
        } else {
            result = intersection(result, hp);
        }
        if (result.empty())
            break;
    }
    return result;
}

double SegmentSupportBounds::floor_z(const Point &p) const
{
    double       z     = - std::numeric_limits<double>::max();
    bool         found = false;
    const double x     = unscale<double>(p.x());
    const double y     = unscale<double>(p.y());
    for (const Plane &pl : m_planes)
        if (! pl.ceiling && std::abs(pl.n.z()) >= NORMAL_EPSILON) {
            z     = std::max(z, (pl.d - pl.n.x() * x - pl.n.y() * y) / pl.n.z());
            found = true;
        }
    // No floor under this point (only vertical floors, or ceilings alone): report 0.0 so the caller
    // treats it as "no floor to descend to" (deepest >= -EPSILON) rather than -inf, which would
    // drive an unbounded downward extension.
    return found ? z : 0.0;
}

Polygons SegmentSupportBounds::half_plane(const Plane &pl, coordf_t print_z, bool segment_side) const
{
    return half_plane_at(pl.n, pl.d, print_z, segment_side);
}

Polygons SegmentSupportBounds::floor_band(coordf_t z_lo, coordf_t z_hi) const
{
    Polygons band;
    if (! m_active)
        return band;
    for (const Plane &pl : m_planes) {
        if (pl.ceiling)
            continue;
        // Above the floor at the top of the slab, below it at the bottom: the floor crosses the slab.
        Polygons above_at_top    = this->half_plane(pl, z_hi, /*segment_side=*/true);
        Polygons below_at_bottom = this->half_plane(pl, z_lo, /*segment_side=*/false);
        if (above_at_top.empty() || below_at_bottom.empty())
            continue;
        append(band, intersection(above_at_top, below_at_bottom));
    }
    return union_(band);
}

Polygons SegmentSupportBounds::half_plane_at(const Vec3d &n, double d, coordf_t print_z, bool inside_side)
{
    const double  gx    = n.x();
    const double  gy    = n.y();
    const double  gnorm = std::hypot(gx, gy);
    const coord_t BIG   = scale_(1e4);   // far beyond any bed; bounds the half-plane for Clipper

    if (gnorm < 1e-9) {
        // Plane (nearly) horizontal in the slice frame: the whole layer is on one side of it.
        // With a tolerance: the root's bed plane is raised by exactly one first-layer height and the
        // first support layer sits at exactly that height, while the segment's build_z_min carries
        // the mesh's microns of noise. An exact test then dropped the whole first support layer.
        const bool inside = (n.z() * double(print_z) >= d - EPSILON);
        if (inside == inside_side) {
            Polygon sq;
            sq.points = { Point(-BIG, -BIG), Point(BIG, -BIG), Point(BIG, BIG), Point(-BIG, BIG) };
            return { sq };
        }
        return {};
    }

    // Boundary line in slice XY: n_x*x + n_y*y = d - n_z*z0. Signed offset along the unit gradient
    // is s; in the frame rotated so the gradient is +X, the segment side is x >= s.
    const double  s_mm = (d - n.z() * double(print_z)) / gnorm;
    const coord_t s    = scale_(s_mm);

    // Degenerate: the split line lies outside the bounded box (e.g. a layer far below the plane),
    // so the whole region is on one side. Avoids an inverted rectangle that Clipper would mishandle.
    if (s >= BIG || s <= -BIG) {
        const bool segment_full = (s <= -BIG);   // entire box satisfies x >= s
        if (segment_full == inside_side) {
            Polygon sq;
            sq.points = { Point(-BIG, -BIG), Point(BIG, -BIG), Point(BIG, BIG), Point(-BIG, BIG) };
            return { sq };
        }
        return {};
    }

    const double ang = std::atan2(gy, gx);
    Polygon      poly;
    if (inside_side)
        poly.points = { Point(s, -BIG), Point(BIG, -BIG), Point(BIG, BIG), Point(s, BIG) };
    else
        poly.points = { Point(-BIG, -BIG), Point(s, -BIG), Point(s, BIG), Point(-BIG, BIG) };
    poly.rotate(ang);   // rotate the axis-aligned rectangle back into the slice XY frame
    return { poly };
}

} // namespace Slic3r
