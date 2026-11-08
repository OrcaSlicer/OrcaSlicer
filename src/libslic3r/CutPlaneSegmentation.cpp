#include "CutPlaneSegmentation.hpp"

#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "Model.hpp"
#include "TriangleMeshSlicer.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <limits>
#include <map>

namespace Slic3r {

// Vertices within this distance of a plane classify as "near" (bed side), so geometry that merely
// touches a plane does not spawn a phantom far-side segment.
static constexpr double CUT_PLANE_EPSILON = EPSILON; // 1e-4 mm
// A vertex this close to a plane counts as lying ON it. Cut-face vertices come out of cut_mesh's
// float rotate-cut-rotate round trip with ~1e-5 mm residue; 1e-3 mm leaves plenty of headroom
// while staying far below any feature size.
static constexpr double CUT_PLANE_CONTACT_EPSILON = 1e-3; // mm

namespace {

// UNLAYERED_MOD -- horizontal extent of a segment, accumulated a vertex at a time because the three
// derivation sites reach their vertices in three different shapes (one merged mesh, one part mesh,
// several pieces of a chunk). Feeds CutSegmentDescriptor::build_reach_max; see the comment there.
struct HorizontalExtent
{
    double xmin { std::numeric_limits<double>::max() },    ymin { std::numeric_limits<double>::max() };
    double xmax { std::numeric_limits<double>::lowest() }, ymax { std::numeric_limits<double>::lowest() };

    void add(const Vec3d &p) {
        xmin = std::min(xmin, p.x()); xmax = std::max(xmax, p.x());
        ymin = std::min(ymin, p.y()); ymax = std::max(ymax, p.y());
    }
    // The horizontal bounding box's DIAGONAL. Two points inside a box are never further apart than
    // its diagonal, so this bounds the run in whatever direction the tilt ends up pointing -- which
    // is what makes it safe to compute once and not recompute when the user spins the plane about
    // Z. Over-estimates a long thin chunk tilted across its short axis; erring strict is the point.
    double reach() const { return xmax < xmin ? 0. : std::hypot(xmax - xmin, ymax - ymin); }
};

// An enabled, unit-normalized plane, possibly flipped so the root cell is all-near.
struct NormalizedPlane
{
    Vec3d  n;                // unit normal, flipped if needed
    double d;                // n.dot(point)
    int    src;              // index into ModelObject::cut_planes.planes
    bool   flipped { false };
};

struct CellStats
{
    // Per plane: minimum signed distance of the cell's vertices to the plane (positive on the far
    // side). Only meaningful for planes on whose far side the cell lies. Chooses the base plane of
    // a FLOATING piece (no contact with any bounding plane).
    std::vector<double> min_far_dist;
    // Per plane: total AREA of the piece's triangles lying ON the plane -- the size of the piece's
    // cut face against it. A materialized piece has cut-face vertices at distance ~0 from EVERY
    // plane that bounds its cell, so min_far_dist ties at ~0 and cannot say which face the piece
    // actually sits on; the largest contact face can. Area, not rim length: the same geometric
    // face triangulates differently depending on which later cuts crossed it, so edge sums differ
    // wildly between equal faces while areas agree.
    std::vector<double> contact_area;
};

// Data derived once per occupied cell from its materialized clipped piece. The seat sources the
// peel-order constraints; the root cell also records its extent here (non-root chunks measure
// theirs over their merged pieces, along their own build normal).
struct CellDerived
{
    int    base { -1 };              // seat: index into `planes`; -1 for the root cell
    double lo   { std::numeric_limits<double>::max() };     // root only: extent along world Z,
    double hi   { std::numeric_limits<double>::lowest() };  // instance frame
    double reach { 0. };             // root only: horizontal extent, CutSegmentDescriptor::build_reach_max
};

// UNLAYERED_MOD -- a non-root segment whose material comes this close to the plate (instance-frame
// z, mm) gets a Bed clip. Generous on purpose: the slicer only removes material below half a layer,
// so for a segment that never gets that low the clip is inert, and a segment within a millimetre of
// the plate is exactly the one whose tilted layers can be driven into it.
static constexpr double BED_CLIP_REACH = 1.0;

// The build plate as a segment clip: instance-frame z = r . p + t_z (r = third row of the linear
// part) must stay >= 0, i.e. (-r) . p - t_z <= 0 in the clip convention.
CutSegmentClip make_bed_clip(const Transform3d &instance_transform)
{
    CutSegmentClip clip;
    clip.normal      = - instance_transform.linear().row(2).transpose();
    clip.offset      = instance_transform.translation().z();
    clip.plane_index = -1;
    clip.role        = CutClipRole::Bed;
    return clip;
}

// Lowest instance-frame z of `its` (object-local).
double min_bed_height(const indexed_triangle_set &its, const Transform3d &instance_transform)
{
    double z = std::numeric_limits<double>::max();
    for (const stl_vertex &v : its.vertices)
        z = std::min(z, (instance_transform * v.cast<double>()).z());
    return z;
}

// Extent along `n_world` of the part of `its` (object-local) on or above the plate. Each triangle
// is clipped against z = 0 first, so a triangle crossing the plate contributes its crossing points
// rather than its submerged corner: the extent of the clipped solid is reached at a vertex above
// the plate or on a crossing edge. Leaves lo/hi untouched when nothing is above the plate.
void extend_above_bed(const indexed_triangle_set &its, const Transform3d &instance_transform,
                      const Vec3d &n_world, double &lo, double &hi)
{
    for (const stl_triangle_vertex_indices &tri : its.indices) {
        Vec3d w[3];
        for (int k = 0; k < 3; ++ k)
            w[k] = instance_transform * its.vertices[tri[k]].cast<double>();
        for (int k = 0; k < 3; ++ k) {
            const Vec3d &a = w[k];
            const Vec3d &b = w[(k + 1) % 3];
            if (a.z() >= 0.) {
                lo = std::min(lo, n_world.dot(a));
                hi = std::max(hi, n_world.dot(a));
            }
            if ((a.z() < 0.) != (b.z() < 0.)) {
                const Vec3d x = a + (a.z() / (a.z() - b.z())) * (b - a);
                lo = std::min(lo, n_world.dot(x));
                hi = std::max(hi, n_world.dot(x));
            }
        }
    }
}

} // anonymous namespace

indexed_triangle_set merged_model_part_mesh(const ModelObject &mo, bool exclude_directed_parts)
{
    // The merged triangle set may contain multiple disconnected shells; cut_mesh handles those as
    // one solid collection.
    indexed_triangle_set merged;
    for (const ModelVolume *volume : mo.volumes) {
        if (! volume->is_model_part())
            continue;
        if (exclude_directed_parts && model_volume_has_build_direction(*volume))
            continue;
        indexed_triangle_set its = volume->mesh().its;
        its_transform(its, volume->get_matrix());
        const int base = int(merged.vertices.size());
        merged.vertices.insert(merged.vertices.end(), its.vertices.begin(), its.vertices.end());
        for (stl_triangle_vertex_indices tri : its.indices) {
            tri.array() += base;
            merged.indices.push_back(tri);
        }
    }
    return merged;
}

bool model_volume_has_build_direction(const ModelVolume &mv)
{
    return mv.is_model_part() && (mv.config.has("layer_rotation_b") || mv.config.has("layer_rotation_c"));
}

bool object_has_directed_parts(const ModelObject &mo)
{
    for (const ModelVolume *mv : mo.volumes)
        if (model_volume_has_build_direction(*mv))
            return true;
    return false;
}

bool segment_includes_volume(const CutSegmentDescriptor *segment, const ModelVolume &mv)
{
    if (segment == nullptr || ! mv.is_model_part())
        return true;
    if (segment->is_part())
        return mv.id() == segment->part_volume;
    return ! model_volume_has_build_direction(mv);
}

void append_part_segments(const ModelObject &mo, const indexed_triangle_set &merged,
                          const Transform3d &instance_transform,
                          std::vector<CutSegmentDescriptor> &segments,
                          std::vector<indexed_triangle_set> *out_meshes)
{
    std::vector<const ModelVolume*> directed;
    for (const ModelVolume *mv : mo.volumes)
        if (model_volume_has_build_direction(*mv))
            directed.push_back(mv);
    if (directed.empty())
        return;

    if (segments.empty() && ! merged.indices.empty()) {
        // No plane partition: the undirected parts are the root on their own. Same bookkeeping
        // as the partition's root -- extent along world +Z in the instance frame.
        CutSegmentDescriptor root;
        root.index       = 0;
        root.build_z_min = std::numeric_limits<double>::max();
        root.build_z_max = std::numeric_limits<double>::lowest();
        HorizontalExtent root_extent;
        for (const stl_vertex &v : merged.vertices) {
            const Vec3d p = instance_transform * v.cast<double>();
            root.build_z_min = std::min(root.build_z_min, p.z());
            root.build_z_max = std::max(root.build_z_max, p.z());
            root_extent.add(p);
        }
        root.build_reach_max = root_extent.reach();
        if (out_meshes)
            out_meshes->assign(1, merged);
        segments.push_back(std::move(root));
    }
    if (out_meshes)
        out_meshes->resize(segments.size());

    for (const ModelVolume *mv : directed) {
        const double b = mv->config.has("layer_rotation_b") ? mv->config.opt_float("layer_rotation_b") : 0.;
        const double c = mv->config.has("layer_rotation_c") ? mv->config.opt_float("layer_rotation_c") : 0.;
        // The angles are world-frame (the machine's B/C, exactly what an object's config holds).
        // The descriptor keeps its build normal object-local like every other segment, so the
        // slicer's inverse-transpose mapping brings back precisely this world direction.
        const Vec3d n_world = Geometry::build_normal_from_bc(b, c).normalized();
        CutSegmentDescriptor seg;
        seg.index        = int(segments.size());
        seg.parent       = -1;
        seg.part_volume  = mv->id();
        seg.build_normal = (instance_transform.linear().transpose() * n_world).normalized();

        indexed_triangle_set its = mv->mesh().its;
        its_transform(its, mv->get_matrix());
        seg.build_z_min = std::numeric_limits<double>::max();
        seg.build_z_max = std::numeric_limits<double>::lowest();
        HorizontalExtent seg_extent;
        for (const stl_vertex &v : its.vertices) {
            const Vec3d p = instance_transform * v.cast<double>();
            const double h = n_world.dot(p);
            seg.build_z_min = std::min(seg.build_z_min, h);
            seg.build_z_max = std::max(seg.build_z_max, h);
            seg_extent.add(p);
        }
        seg.build_reach_max = seg_extent.reach();
        if (its.vertices.empty())
            seg.build_z_min = seg.build_z_max = 0.;
        else if (min_bed_height(its, instance_transform) < BED_CLIP_REACH) {
            // A directed part rests on nothing the partition knows about, so it may well stand on
            // the plate with its layers tilted into it. See CutClipRole::Bed.
            double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
            extend_above_bed(its, instance_transform, n_world, lo, hi);
            seg.clips.push_back(make_bed_clip(instance_transform));
            if (lo <= hi) {
                seg.build_z_min = lo;
                seg.build_z_max = hi;
            }
        }
        if (out_meshes)
            out_meshes->push_back(std::move(its));
        segments.push_back(std::move(seg));
    }
}

std::vector<CutSegmentDescriptor> derive_cut_segments(
    const ModelObject &mo, const Transform3d &instance_transform, double min_segment_thickness,
    CutSegmentationStatus *status, std::vector<indexed_triangle_set> *out_meshes,
    const Vec3d &root_build_normal_world)
{
    if (out_meshes)
        out_meshes->clear();
    const bool directed = object_has_directed_parts(mo);
    if (! mo.cut_planes.active() && ! directed) {
        if (status)
            *status = CutSegmentationStatus::StackInactive;
        return {};
    }
    // Directed parts never take part in the plane partition: they are their own segments.
    const indexed_triangle_set merged = merged_model_part_mesh(mo, /*exclude_directed_parts*/ true);
    std::vector<CutSegmentDescriptor> segments;
    if (mo.cut_planes.active())
        segments = derive_cut_segments(merged, mo.cut_planes.planes, instance_transform,
                                       min_segment_thickness, mo.cut_planes.user_ordered, status, out_meshes, mo.name,
                                       root_build_normal_world);
    else if (status)
        *status = CutSegmentationStatus::StackInactive;
    if (directed) {
        append_part_segments(mo, merged, instance_transform, segments, out_meshes);
        if (status && ! segments.empty() && *status == CutSegmentationStatus::StackInactive)
            *status = CutSegmentationStatus::Ok;
    }
    return segments;
}

std::vector<CutSegmentDescriptor> derive_cut_segments(
    const indexed_triangle_set &merged, const std::vector<CutPlane> &authored_planes,
    const Transform3d &instance_transform, double min_segment_thickness, bool user_ordered,
    CutSegmentationStatus *status, std::vector<indexed_triangle_set> *out_meshes,
    const std::string &object_name, const Vec3d &root_build_normal_world)
{
    if (out_meshes)
        out_meshes->clear();
    const auto fail = [status](CutSegmentationStatus s) {
        if (status)
            *status = s;
        return std::vector<CutSegmentDescriptor>{};
    };

    // Collect enabled planes with well-defined normals.
    std::vector<NormalizedPlane> planes;
    for (int i = 0; i < int(authored_planes.size()); ++ i) {
        const CutPlane &cp = authored_planes[i];
        if (! cp.enabled)
            continue;
        const double len = cp.normal.norm();
        if (len < EPSILON)
            continue;
        const Vec3d n = cp.normal / len;
        planes.push_back({ n, n.dot(cp.point), i, false });
    }
    // A duplicate of an earlier plane -- the same geometric plane, either facing -- is dropped:
    // the first authored plane wins (its src, its facing). Without this an exact duplicate can
    // shave a float-thin sliver on the second cut of the same plane (tripping the thin-segment
    // gate), and an anti-parallel duplicate of a vertical plane leaves no all-near root cell.
    {
        size_t kept = 0;
        for (size_t j = 0; j < planes.size(); ++ j) {
            bool duplicate = false;
            for (size_t i = 0; i < kept && ! duplicate; ++ i) {
                const double dot = planes[i].n.dot(planes[j].n);
                if (std::abs(dot) > 1. - 1e-9 &&
                    std::abs(planes[i].d - (dot > 0. ? planes[j].d : -planes[j].d)) <= CUT_PLANE_EPSILON)
                    duplicate = true;
            }
            if (duplicate)
                BOOST_LOG_TRIVIAL(debug) << "derive_cut_segments: dropping duplicate cut plane "
                                         << planes[j].src << " on object '" << object_name << "'";
            else
                planes[kept ++] = planes[j];
        }
        planes.resize(kept);
    }
    if (planes.empty())
        return fail(CutSegmentationStatus::StackInactive);
    if (planes.size() > 64) {
        BOOST_LOG_TRIVIAL(warning) << "derive_cut_segments: more than 64 enabled cut planes on object '"
                                   << object_name << "', declarative segmentation disabled for it";
        return fail(CutSegmentationStatus::TooManyPlanes);
    }
    if (merged.vertices.empty())
        return fail(CutSegmentationStatus::NoGeometry);

    std::vector<Vec3d> verts;
    verts.reserve(merged.vertices.size());
    for (const stl_vertex &v : merged.vertices)
        verts.emplace_back(v.cast<double>());

    // Normalize plane orientations: a plane whose far side reaches STRICTLY closer to the bed
    // (lowest world z under this PrintObject's instance transform) than its
    // near side is flipped, so the bed-resting geometry is on the near side regardless of how the
    // plane was authored. Ties (e.g. a vertical plane — both sides touch the bed) keep the
    // authored orientation: which side prints first is then the author's choice.
    std::vector<double> world_z;
    world_z.reserve(verts.size());
    for (const Vec3d &v : verts)
        world_z.push_back((instance_transform * v).z());
    for (NormalizedPlane &pl : planes) {
        double near_min = std::numeric_limits<double>::max();
        double far_min  = std::numeric_limits<double>::max();
        for (size_t vi = 0; vi < verts.size(); ++ vi) {
            if (pl.n.dot(verts[vi]) - pl.d > CUT_PLANE_EPSILON)
                far_min = std::min(far_min, world_z[vi]);
            else
                near_min = std::min(near_min, world_z[vi]);
        }
        if (far_min < near_min - CUT_PLANE_EPSILON) {
            pl.n = -pl.n;
            pl.d = -pl.d;
            pl.flipped = true;
        }
    }

    // Split the actual solid successively by every plane. Sampling only original vertices/edges
    // misses occupied cells enclosed wholly inside the mesh; materializing the clipped pieces
    // guarantees that every non-empty arrangement cell receives a descriptor and that descriptor
    // discovery uses the same geometry operation as the chunk preview.
    std::map<uint64_t, CellStats> cells;
    struct Piece { uint64_t mask; indexed_triangle_set mesh; };
    std::vector<Piece> pieces { { 0, merged } };
    for (size_t j = 0; j < planes.size(); ++ j) {
        std::vector<Piece> next;
        next.reserve(pieces.size() * 2);
        const NormalizedPlane &pl = planes[j];
        const Eigen::Matrix3d rot = Eigen::Quaterniond::FromTwoVectors(pl.n, Vec3d::UnitZ()).toRotationMatrix();
        for (Piece &piece : pieces) {
            its_transform(piece.mesh, Transform3d(rot));
            indexed_triangle_set upper, lower;
            cut_mesh(piece.mesh, float(pl.d), &upper, &lower);
            if (! lower.indices.empty()) {
                its_transform(lower, Transform3d(rot.transpose()));
                next.push_back({ piece.mask, std::move(lower) });
            }
            if (! upper.indices.empty()) {
                its_transform(upper, Transform3d(rot.transpose()));
                next.push_back({ piece.mask | (uint64_t(1) << j), std::move(upper) });
            }
        }
        pieces = std::move(next);
    }
    for (const Piece &piece : pieces) {
        CellStats &stats = cells[piece.mask];
        stats.min_far_dist.assign(planes.size(), std::numeric_limits<double>::max());
        stats.contact_area.assign(planes.size(), 0.);
        for (const stl_vertex &v : piece.mesh.vertices) {
            const Vec3d p = v.cast<double>();
            for (size_t j = 0; j < planes.size(); ++ j)
                if (piece.mask & (uint64_t(1) << j))
                    stats.min_far_dist[j] = std::min(stats.min_far_dist[j],
                                                     std::max(0., planes[j].n.dot(p) - planes[j].d));
        }
        // Contact face: triangles whose three vertices all lie on a bounding plane.
        for (const stl_triangle_vertex_indices &tri : piece.mesh.indices) {
            const Vec3d a = piece.mesh.vertices[tri[0]].cast<double>();
            const Vec3d b = piece.mesh.vertices[tri[1]].cast<double>();
            const Vec3d c = piece.mesh.vertices[tri[2]].cast<double>();
            for (size_t j = 0; j < planes.size(); ++ j) {
                if (! (piece.mask & (uint64_t(1) << j)))
                    continue;
                if (std::abs(planes[j].n.dot(a) - planes[j].d) <= CUT_PLANE_CONTACT_EPSILON &&
                    std::abs(planes[j].n.dot(b) - planes[j].d) <= CUT_PLANE_CONTACT_EPSILON &&
                    std::abs(planes[j].n.dot(c) - planes[j].d) <= CUT_PLANE_CONTACT_EPSILON)
                    stats.contact_area[j] += 0.5 * (b - a).cross(c - a).norm();
            }
        }
    }
    std::map<uint64_t, CellDerived> derived;
    // Per-cell seat, computed while the materialized pieces are still in hand.
    for (const Piece &piece : pieces) {
        const CellStats &stats = cells[piece.mask];
        CellDerived      d;
        if (piece.mask != 0) {
            // The seat is the plane the piece actually SITS on: the bounding plane with the
            // largest cut-face contact. min_far_dist cannot decide this -- a materialized piece
            // touches every plane that bounds it (its cut-face vertices lie on them), so distances
            // tie at ~0 and the choice would degenerate to plane order; measured on a real part,
            // that built a large far-of-both segment along the wrong plane's normal and hung it
            // off a millimetre-thin sibling. A piece with no contact at all FLOATS: for it the
            // closest far-side plane is the right seat. Remaining ties keep plane order.
            double best_area = 0.;
            for (size_t j = 0; j < planes.size(); ++ j)
                if ((piece.mask & (uint64_t(1) << j)) && stats.contact_area[j] > best_area) {
                    best_area = stats.contact_area[j];
                    d.base    = int(j);
                }
            if (d.base < 0) {
                double best = std::numeric_limits<double>::max();
                for (size_t j = 0; j < planes.size(); ++ j)
                    if ((piece.mask & (uint64_t(1) << j)) && stats.min_far_dist[j] < best) {
                        best   = stats.min_far_dist[j];
                        d.base = int(j);
                    }
            }
        } else {
            // The root builds along the machine's build direction (world +Z on a flat bed, the
            // gantry normal on a belt); its extent feeds the slice-frame rebase and the
            // thin-segment check directly.
            HorizontalExtent extent;
            for (const stl_vertex &v : piece.mesh.vertices) {
                const Vec3d  p = instance_transform * v.cast<double>();
                // BELT_MOD: heights along the root's build direction (world +Z off a belt printer).
                const double h = root_build_normal_world.dot(p);
                d.lo = std::min(d.lo, h);
                d.hi = std::max(d.hi, h);
                extent.add(p);
            }
            d.reach = extent.reach();
        }
        derived[piece.mask] = d;
    }
    if (cells.size() < 2)
        // No enabled plane actually splits the mesh: feature inert.
        return fail(CutSegmentationStatus::NoSplit);
    if (cells.count(0) == 0) {
        // No cell is on the near side of every plane — e.g. two vertical planes whose all-near
        // quadrant holds no geometry. There is no well-defined bed-resting root then; leave the
        // feature inert rather than guessing a print order.
        BOOST_LOG_TRIVIAL(warning) << "derive_cut_segments: no root (all-near) cell for object '"
                                   << object_name << "', declarative segmentation disabled for it";
        return fail(CutSegmentationStatus::NoRootCell);
    }

    // A plane produces clips only when it actually splits the mesh (some cell on each side). After
    // normalization every plane has a near cell (the root), so "splitting" == "has a far cell".
    uint64_t splitting = 0;
    for (const auto &[mask, stats] : cells)
        splitting |= mask;

    // ---- Peel order ----
    // One chunk per splitting plane: the planes are ordered, and chunk k is everything on the far
    // side of plane k that no later plane claims (far(k) restricted to the near side of every
    // later plane). N planes therefore yield at most N + 1 sections -- the sequential-cut
    // semantics a user expects -- rather than one section per arrangement cell, which turned two
    // planes crossing inside the object into FOUR sections (the far-of-both wedge became its own
    // segment instead of merging into the chunk it sits on).
    //
    // The order is chosen so every cell lands in the chunk of the plane it SITS on (its seat): the
    // seat must peel after every other plane bounding the cell, or a later plane would claim the
    // cell. Kahn's algorithm over those constraints; among unconstrained planes the LAST-authored
    // one peels first, so the first-authored cut prints last (the established sibling order). Any
    // order yields a printable partition -- the material across a chunk's floor face belongs to
    // the root or to earlier chunks by construction -- so on a constraint cycle (three or more
    // mutually crossing planes with rotationally arranged seats) the sort just proceeds and the
    // affected wedge builds along a plane it merely touches instead of its seat.
    // Only a DECISIVELY larger seat face imposes an order. A tied or near-tied cell (equal cut
    // faces -- e.g. the corner cell of a plane grid) constrains nothing: any of its planes is an
    // equally good owner, and hard constraints manufactured from the tie-break would fabricate
    // cycles between planes whose order is actually free.
    //
    // A user-ordered stack skips the seat constraints entirely: the tie-break alone (highest index
    // peels first) then realizes the pure list order -- the first plane in the list peels last,
    // prints last, and claims any contested wedge. That is the manual override for exactly the
    // case where the seat rule would pin the order and make the reorder buttons inert.
    constexpr double SEAT_DECISIVE = 1.05;
    std::vector<uint64_t> preds(planes.size(), 0);   // preds[s]: planes that must peel before s
    if (! user_ordered)
        for (const auto &[mask, stats] : cells) {
            if (mask == 0 || (mask & (mask - 1)) == 0)
                continue;                             // root, or a single far plane: no constraint
            const int s = derived.at(mask).base;
            assert(s >= 0);
            if (stats.contact_area[s] <= 0.)
                continue;                             // floating cell: no seat face to defend
            for (size_t j = 0; j < planes.size(); ++ j)
                if (int(j) != s && (mask & (uint64_t(1) << j))
                    && stats.contact_area[s] > stats.contact_area[j] * SEAT_DECISIVE)
                    preds[s] |= uint64_t(1) << j;
        }
    std::vector<int> order;                           // plane indices in peel (= print) order
    order.reserve(planes.size());
    {
        uint64_t placed = 0;
        uint64_t remaining = splitting;
        while (remaining != 0) {
            int pick = -1;
            for (int j = 0; j < int(planes.size()); ++ j)
                if ((remaining & (uint64_t(1) << j)) && (preds[j] & ~placed) == 0)
                    pick = j;                         // the highest ready index wins the tie
            if (pick < 0) {
                for (int j = int(planes.size()) - 1; j >= 0 && pick < 0; -- j)
                    if (remaining & (uint64_t(1) << j))
                        pick = j;                     // constraint cycle: forced pick (see above)
                BOOST_LOG_TRIVIAL(warning) << "derive_cut_segments: cyclic seat constraints on object '"
                                           << object_name << "', peel order forced for plane "
                                           << planes[pick].src;
            }
            order.push_back(pick);
            placed    |= uint64_t(1) << pick;
            remaining &= ~(uint64_t(1) << pick);
        }
    }
    std::vector<int> pos(planes.size(), -1);          // peel position of each splitting plane
    for (size_t k = 0; k < order.size(); ++ k)
        pos[order[k]] = int(k);
    // The chunk owning a cell is the latest-peeling plane bounding it. With every seat constraint
    // satisfied that IS the cell's seat.
    const auto owner_position = [&planes, &pos](uint64_t mask) -> int {
        int latest = -1;
        for (size_t j = 0; j < planes.size(); ++ j)
            if ((mask & (uint64_t(1) << j)) && (latest < 0 || pos[j] > pos[latest]))
                latest = int(j);
        return latest < 0 ? -1 : pos[latest];
    };

    // Index of each cell's materialized piece, so the caller can be handed the very meshes this
    // derivation cut -- rather than reconstructing the partition a second time from the merged mesh
    // (which cuts each physical plane twice, once per side, in two different rotated frames).
    std::map<uint64_t, size_t> piece_of_mask;
    for (size_t i = 0; i < pieces.size(); ++ i)
        piece_of_mask[pieces[i].mask] = i;

    // ---- Group cells into chunks and derive each chunk's data over its merged pieces ----
    struct Chunk
    {
        int                   plane { -1 };           // index into `planes`
        std::vector<uint64_t> cells;                  // constituent occupied cell masks
        Vec3d                 build_normal_local { Vec3d::UnitZ() };
        Vec3d                 build_normal_world { Vec3d::UnitZ() };
        double                lo  { std::numeric_limits<double>::max() };     // extent along the
        double                hi  { std::numeric_limits<double>::lowest() };  // build axis
        double                gap { 0. };             // > 0 when the chunk floats above its plane
        HorizontalExtent      extent;                 // -> CutSegmentDescriptor::build_reach_max
        uint64_t              seat_cell { 0 };        // largest cut face against `plane`
        int                   seg { -1 };             // emitted segment index
        // Reaches the plate (see CutClipRole::Bed): extent along the build axis of the part above
        // it. lo/hi above stay the full extent, which the thin-segment check keeps using -- a
        // chunk mostly under the plate must not disable the whole partition, it just slices thin.
        bool                  bed { false };
        double                bed_lo { std::numeric_limits<double>::max() };
        double                bed_hi { std::numeric_limits<double>::lowest() };
    };
    std::vector<Chunk> chunks(order.size());
    for (size_t k = 0; k < order.size(); ++ k)
        chunks[k].plane = order[k];
    for (const auto &[mask, stats] : cells)
        if (mask != 0)
            chunks[owner_position(mask)].cells.push_back(mask);   // std::map order: deterministic
    for (Chunk &chunk : chunks) {
        if (chunk.cells.empty())
            continue;   // every cell far of this plane was claimed by a later one
        const int       j        = chunk.plane;
        const CutPlane &authored = authored_planes[planes[j].src];
        chunk.build_normal_local = planes[j].n;
        if (authored.has_build_normal && authored.build_normal.norm() > EPSILON)
            chunk.build_normal_local = authored.build_normal.normalized();
        chunk.build_normal_world =
            (instance_transform.linear().inverse().transpose() * chunk.build_normal_local).normalized();
        const Vec3d &build_normal_world = chunk.build_normal_world;
        // Seat: the constituent cell with the largest cut face against the chunk's own plane; when
        // none touches it (the chunk floats), the one closest to it. Ties keep the first cell.
        double best_contact = 0.;
        chunk.seat_cell = chunk.cells.front();
        for (const uint64_t mask : chunk.cells)
            if (const CellStats &stats = cells.at(mask); stats.contact_area[j] > best_contact) {
                best_contact    = stats.contact_area[j];
                chunk.seat_cell = mask;
            }
        if (best_contact <= 0.) {
            double best_dist = std::numeric_limits<double>::max();
            for (const uint64_t mask : chunk.cells)
                if (const CellStats &stats = cells.at(mask); stats.min_far_dist[j] < best_dist) {
                    best_dist       = stats.min_far_dist[j];
                    chunk.seat_cell = mask;
                }
        }
        // Extent along the build axis (instance frame) and the perpendicular gap down to the
        // chunk's plane, measured over ALL constituent pieces. Normally the gap is 0 -- the plane
        // cut a face here. Positive means the chunk FLOATS above its plane, which happens when the
        // plane passes through empty space between disconnected shells.
        double nearest = std::numeric_limits<double>::max();
        double lowest  = std::numeric_limits<double>::max();
        for (const uint64_t mask : chunk.cells)
            for (const stl_vertex &v : pieces[piece_of_mask.at(mask)].mesh.vertices) {
                const Vec3d p = v.cast<double>();
                const Vec3d w = instance_transform * p;
                const double h = build_normal_world.dot(w);
                chunk.lo = std::min(chunk.lo, h);
                chunk.hi = std::max(chunk.hi, h);
                chunk.extent.add(w);
                nearest  = std::min(nearest, std::max(0., planes[j].n.dot(p) - planes[j].d));
                lowest   = std::min(lowest, w.z());
            }
        chunk.gap = nearest <= CUT_PLANE_CONTACT_EPSILON ? 0. : nearest;
        if (lowest < BED_CLIP_REACH) {
            chunk.bed = true;
            for (const uint64_t mask : chunk.cells)
                extend_above_bed(pieces[piece_of_mask.at(mask)].mesh, instance_transform, build_normal_world,
                                 chunk.bed_lo, chunk.bed_hi);
        }
    }
    if (min_segment_thickness > 0.) {
        // Dropping just one chunk would lose material from the partition. Keep the feature inert
        // for this transform until every derived section can contain at least its first layer.
        if (derived.at(0).hi - derived.at(0).lo + CUT_PLANE_EPSILON < min_segment_thickness)
            return fail(CutSegmentationStatus::SegmentTooThin);
        for (const Chunk &chunk : chunks)
            if (! chunk.cells.empty() && chunk.hi - chunk.lo + CUT_PLANE_EPSILON < min_segment_thickness)
                return fail(CutSegmentationStatus::SegmentTooThin);
    }

    // ---- Emit the descriptors: root first, then one segment per non-empty chunk in peel order.
    // A parent always precedes its children (every cell across a chunk's floor is bounded only by
    // earlier-peeling planes), which Print.cpp's postorder generation relies on.
    int next_index = 1;
    for (Chunk &chunk : chunks)
        if (! chunk.cells.empty())
            chunk.seg = next_index ++;
    const auto segment_of_cell = [&chunks, &owner_position](uint64_t mask) -> int {
        return mask == 0 ? 0 : chunks[owner_position(mask)].seg;
    };
    const auto append_piece = [](indexed_triangle_set &dst, indexed_triangle_set &&src) {
        if (dst.indices.empty()) {
            dst = std::move(src);
            return;
        }
        const int base = int(dst.vertices.size());
        dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());
        dst.indices.reserve(dst.indices.size() + src.indices.size());
        for (stl_triangle_vertex_indices tri : src.indices) {
            tri.array() += base;
            dst.indices.push_back(tri);
        }
    };
    if (out_meshes)
        out_meshes->resize(next_index);

    // Where the material on the FAR side of each splitting plane lands on that plane, object-local:
    // every vertex of every far-side piece projected along the plane normal. A later segment's
    // support and body columns come down onto its base plane over exactly that footprint, so a
    // segment sitting below the plane may have to carry support up to the highest of these points
    // along its own build axis (CutSegmentDescriptor::support_z_max). Projection rather than the
    // cut-face vertices alone: a segment floating above a plane through open space has no cut
    // face, yet its column still has to be met at the plane. Collected here, before the emission
    // loop moves the piece meshes out.
    std::vector<std::vector<Vec3d>> on_plane(planes.size());
    for (size_t j = 0; j < planes.size(); ++ j) {
        if ((splitting & (uint64_t(1) << j)) == 0)
            continue;
        for (const Piece &piece : pieces) {
            if ((piece.mask & (uint64_t(1) << j)) == 0)
                continue;               // near side: not carried across this plane
            for (const stl_vertex &v : piece.mesh.vertices) {
                const Vec3d p = v.cast<double>();
                on_plane[j].push_back(p - planes[j].n * (planes[j].n.dot(p) - planes[j].d));
            }
        }
    }
    // The reach of ceiling plane `j` for a segment bounded by `clips`: only the part of its cut
    // face inside the segment's OTHER bounds counts. Where a second ceiling lies below this one
    // the segment cannot reach this face anyway (the root under two crossing planes), and a
    // non-root segment only owns the face on the far side of its own floor.
    const auto ceiling_reach = [&on_plane](size_t j, const std::vector<CutSegmentClip> &clips, int src_j, const auto &height_of) {
        double reach = std::numeric_limits<double>::lowest();
        for (const Vec3d &p : on_plane[j]) {
            bool inside = true;
            for (const CutSegmentClip &clip : clips)
                if (clip.plane_index != src_j && clip.normal.dot(p) - clip.offset > CUT_PLANE_CONTACT_EPSILON) {
                    inside = false;
                    break;
                }
            if (inside)
                reach = std::max(reach, height_of(p));
        }
        return reach;
    };

    std::vector<CutSegmentDescriptor> segments;
    segments.reserve(next_index);
    {
        // Root: the near side of every splitting plane. Every clip is a Ceiling -- support must
        // stay below a plane whose far side prints later, and must never stem from its cut face.
        CutSegmentDescriptor seg;
        seg.index           = 0;
        // BELT_MOD -- stored object-local like every other build normal (covector: M^T of the world
        // direction, the inverse of the inverse-transpose the readers apply).
        seg.build_normal    = (instance_transform.linear().transpose() * root_build_normal_world).normalized();
        seg.root_builds_up  = (root_build_normal_world.normalized() - Vec3d::UnitZ()).norm() < EPSILON;
        seg.build_z_min     = derived.at(0).lo;
        seg.build_z_max     = derived.at(0).hi;
        seg.build_reach_max = derived.at(0).reach;
        seg.support_z_max   = seg.build_z_max;
        for (size_t j = 0; j < planes.size(); ++ j)
            if (splitting & (uint64_t(1) << j))
                seg.clips.push_back({ planes[j].n, planes[j].d, planes[j].src, CutClipRole::Ceiling });
        for (size_t j = 0; j < planes.size(); ++ j)
            if (splitting & (uint64_t(1) << j))
                seg.support_z_max = std::max(seg.support_z_max,
                    ceiling_reach(j, seg.clips, planes[j].src, [&instance_transform](const Vec3d &p) { return (instance_transform * p).z(); }));
        if (out_meshes)
            (*out_meshes)[0] = std::move(pieces[piece_of_mask.at(0)].mesh);
        segments.push_back(std::move(seg));
    }
    for (size_t k = 0; k < chunks.size(); ++ k) {
        Chunk &chunk = chunks[k];
        if (chunk.cells.empty())
            continue;
        const int j = chunk.plane;
        CutSegmentDescriptor seg;
        seg.index        = chunk.seg;
        seg.base_plane   = planes[j].src;
        seg.build_normal = chunk.build_normal_local;
        seg.build_z_min  = chunk.lo;
        seg.build_z_max  = chunk.hi;
        seg.build_reach_max = chunk.extent.reach();
        seg.support_z_max = chunk.hi;
        seg.base_plane_distance_below_material = chunk.gap;
        const Vec3d &build_normal_world = chunk.build_normal_world;   // the axis build_z_min/max were measured along
        // Parent: the segment across the seat's floor face -- the root when that cell is
        // unoccupied. The support cascade hands this chunk's leftover demand to it; a merged
        // chunk's floor can border several earlier segments, and the seat picks the one carrying
        // the largest share.
        const uint64_t across = chunk.seat_cell & ~(uint64_t(1) << j);
        seg.parent = cells.count(across) != 0 ? segment_of_cell(across) : 0;
        assert(seg.parent >= 0 && seg.parent < seg.index);
        // Floor through its own plane (keep the far side: the negated plane), Ceiling through
        // every LATER-peeling plane. Planes peeled earlier do not clip it -- the chunk may
        // legitimately contain material on their far side (that is exactly the merge).
        for (size_t j2 = 0; j2 < planes.size(); ++ j2) {
            if ((splitting & (uint64_t(1) << j2)) == 0)
                continue;
            if (int(j2) == j)
                seg.clips.push_back({ -planes[j2].n, -planes[j2].d, planes[j2].src, CutClipRole::Floor });
            else if (pos[j2] > int(k))
                seg.clips.push_back({ planes[j2].n, planes[j2].d, planes[j2].src, CutClipRole::Ceiling });
        }
        if (chunk.bed) {
            seg.clips.push_back(make_bed_clip(instance_transform));
            if (chunk.bed_lo <= chunk.bed_hi) {
                seg.build_z_min = chunk.bed_lo;
                seg.build_z_max = chunk.bed_hi;
            }
        }
        for (const CutSegmentClip &clip : seg.clips)
            if (clip.role == CutClipRole::Ceiling)
                for (size_t j2 = 0; j2 < planes.size(); ++ j2)
                    if (planes[j2].src == clip.plane_index)
                        seg.support_z_max = std::max(seg.support_z_max,
                            ceiling_reach(j2, seg.clips, clip.plane_index, [&](const Vec3d &p) { return build_normal_world.dot(instance_transform * p); }));
        if (out_meshes) {
            indexed_triangle_set &dst = (*out_meshes)[seg.index];
            for (const uint64_t mask : chunk.cells)
                append_piece(dst, std::move(pieces[piece_of_mask.at(mask)].mesh));
        }
        segments.push_back(std::move(seg));
    }

    if (status)
        *status = CutSegmentationStatus::Ok;
    return segments;
}

ExPolygons clip_expolygons_by_half_plane(ExPolygons &&src, const Vec2d &ab, double c, const BoundingBox &bbox)
{
    if (src.empty())
        return std::move(src);
    const double len = ab.norm();
    if (len < EPSILON)
        // Degenerate: the caller decides between keep-all and drop-all from the sign of c.
        return std::move(src);
    const Vec2d  n = ab / len;
    const double d = c / len;

    // A rectangle on the keep side (n.dot(p) <= d), sized to safely cover bbox.
    const Vec2d bb_min = unscale(bbox.min);
    const Vec2d bb_max = unscale(bbox.max);
    const Vec2d center = 0.5 * (bb_min + bb_max);
    const Vec2d p0     = n * d; // closest point of the boundary line to the origin
    const double L     = (center - p0).norm() + (bb_max - bb_min).norm() + 10.0;

    const Vec2d t(-n.y(), n.x());
    Polygon rect;
    rect.points = {
        Point::new_scale(p0 + t * L),
        Point::new_scale(p0 + t * L - n * (2. * L)),
        Point::new_scale(p0 - t * L - n * (2. * L)),
        Point::new_scale(p0 - t * L),
    };
    // CCW as required for a Slic3r contour.
    assert(rect.is_counter_clockwise());

    return intersection_ex(src, Polygons { rect });
}

std::vector<indexed_triangle_set> segment_meshes(const indexed_triangle_set &merged, const std::vector<CutSegmentDescriptor> &segs)
{
    std::vector<indexed_triangle_set> out;
    out.reserve(segs.size());
    for (const CutSegmentDescriptor &seg : segs) {
        indexed_triangle_set its = merged;
        for (const CutSegmentClip &clip : seg.clips) {
            if (its.indices.empty())
                break;
            // Rotate the clip normal onto +Z, cut at z == offset, keep the lower half
            // (normal.dot(p) <= offset), rotate back. Same recipe as CutUtils::process_volume_cut.
            const Eigen::Matrix3d rot = Eigen::Quaterniond::FromTwoVectors(clip.normal, Vec3d::UnitZ()).toRotationMatrix();
            its_transform(its, Transform3d(rot));
            indexed_triangle_set upper, lower;
            cut_mesh(its, float(clip.offset), &upper, &lower);
            its = std::move(lower);
            its_transform(its, Transform3d(rot.transpose()));
        }
        out.push_back(std::move(its));
    }
    return out;
}

} // namespace Slic3r
