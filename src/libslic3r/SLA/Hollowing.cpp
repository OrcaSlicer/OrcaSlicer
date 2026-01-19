#include <functional>
#include <optional>

#include <libslic3r/OpenVDBUtils.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>
#include <libslic3r/SLA/Hollowing.hpp>
#include <libslic3r/SLA/IndexedMesh.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/QuadricEdgeCollapse.hpp>
#include <libslic3r/SLA/SupportTreeMesher.hpp>

#include <boost/log/trivial.hpp>

#include <libslic3r/MTUtils.hpp>
#include <libslic3r/I18N.hpp>

//! macro used to mark string used at localization,
//! return same string
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {
namespace sla {

struct Interior {
    indexed_triangle_set mesh;
    openvdb::FloatGrid::Ptr gridptr;
    mutable std::optional<openvdb::FloatGrid::ConstAccessor> accessor;

    double closing_distance = 0.;
    double thickness = 0.;
    double voxel_scale = 1.;
    double nb_in = 3.;  // narrow band width inwards
    double nb_out = 3.; // narrow band width outwards
    // Full narrow band is the sum of the two above values.

    void reset_accessor() const  // This resets the accessor and its cache
    // Not a thread safe call!
    {
        if (gridptr)
            accessor = gridptr->getConstAccessor();
    }
};

void InteriorDeleter::operator()(Interior *p)
{
    delete p;
}

indexed_triangle_set &get_mesh(Interior &interior)
{
    return interior.mesh;
}

const indexed_triangle_set &get_mesh(const Interior &interior)
{
    return interior.mesh;
}

static InteriorPtr generate_interior_verbose(const TriangleMesh & mesh,
                                             const JobController &ctl,
                                             double min_thickness,
                                             double voxel_scale,
                                             double closing_dist)
{
    double offset = voxel_scale * min_thickness;
    double D = voxel_scale * closing_dist;
    float  out_range = 0.1f * float(offset);
    float  in_range = 1.1f * float(offset + D);

    if (ctl.stopcondition()) return {};
    else ctl.statuscb(0, L("Hollowing"));

    auto gridptr = mesh_to_grid(mesh.its, {}, voxel_scale, out_range, in_range);

    assert(gridptr);

    if (!gridptr) {
        BOOST_LOG_TRIVIAL(error) << "Returned OpenVDB grid is NULL";
        return {};
    }

    if (ctl.stopcondition()) return {};
    else ctl.statuscb(30, L("Hollowing"));

    double iso_surface = D;
    auto   narrowb = double(in_range);
    gridptr = redistance_grid(*gridptr, -(offset + D), narrowb, narrowb);

    if (ctl.stopcondition()) return {};
    else ctl.statuscb(70, L("Hollowing"));

    double adaptivity = 0.;
    InteriorPtr interior = InteriorPtr{new Interior{}};

    interior->mesh = grid_to_mesh(*gridptr, iso_surface, adaptivity);
    interior->gridptr = gridptr;

    if (ctl.stopcondition()) return {};
    else ctl.statuscb(100, L("Hollowing"));

    interior->closing_distance = D;
    interior->thickness = offset;
    interior->voxel_scale = voxel_scale;
    interior->nb_in = narrowb;
    interior->nb_out = narrowb;

    return interior;
}

InteriorPtr generate_interior(const TriangleMesh &   mesh,
                              const HollowingConfig &hc,
                              const JobController &  ctl)
{
    static const double MIN_OVERSAMPL = 3.5;
    static const double MAX_OVERSAMPL = 8.;

    // I can't figure out how to increase the grid resolution through openvdb
    // API so the model will be scaled up before conversion and the result
    // scaled down. Voxels have a unit size. If I set voxelSize smaller, it
    // scales the whole geometry down, and doesn't increase the number of
    // voxels.
    //
    // max 8x upscale, min is native voxel size
    auto voxel_scale = MIN_OVERSAMPL + (MAX_OVERSAMPL - MIN_OVERSAMPL) * hc.quality;

    InteriorPtr interior =
        generate_interior_verbose(mesh, ctl, hc.min_thickness, voxel_scale,
                                  hc.closing_distance);

    if (interior && !interior->mesh.empty()) {

        // flip normals back...
        swap_normals(interior->mesh);

        // simplify mesh lossless
        float loss_less_max_error = 2*std::numeric_limits<float>::epsilon();
        its_quadric_edge_collapse(interior->mesh, 0U, &loss_less_max_error);

        its_compactify_vertices(interior->mesh);
        its_merge_vertices(interior->mesh);

        // flip normals back...
        swap_normals(interior->mesh);
    }

    return interior;
}

indexed_triangle_set DrainHole::to_mesh() const
{
    auto r = double(radius);
    auto h = double(height);
    indexed_triangle_set hole = sla::cylinder(r, h, steps);
    Eigen::Quaternionf q;
    q.setFromTwoVectors(Vec3f{0.f, 0.f, 1.f}, normal);
    for(auto& p : hole.vertices) p = q * p + pos;
    
    return hole;
}

bool DrainHole::operator==(const DrainHole &sp) const
{
    return (pos == sp.pos) && (normal == sp.normal) &&
            is_approx(radius, sp.radius) &&
            is_approx(height, sp.height);
}

bool DrainHole::is_inside(const Vec3f& pt) const
{
    Eigen::Hyperplane<float, 3> plane(normal, pos);
    float dist = plane.signedDistance(pt);
    if (dist < float(EPSILON) || dist > height)
        return false;

    Eigen::ParametrizedLine<float, 3> axis(pos, normal);
    if ( axis.squaredDistance(pt) < pow(radius, 2.f))
        return true;

    return false;
}


// Given a line s+dir*t, find parameter t of intersections with the hole
// and the normal (points inside the hole). Outputs through out reference,
// returns true if two intersections were found.
bool DrainHole::get_intersections(const Vec3f& s, const Vec3f& dir,
                                  std::array<std::pair<float, Vec3d>, 2>& out)
                                  const
{
    assert(is_approx(normal.norm(), 1.f));
    const Eigen::ParametrizedLine<float, 3> ray(s, dir.normalized());

    for (size_t i=0; i<2; ++i)
        out[i] = std::make_pair(sla::IndexedMesh::hit_result::infty(), Vec3d::Zero());

    const float sqr_radius = pow(radius, 2.f);

    // first check a bounding sphere of the hole:
    Vec3f center = pos+normal*height/2.f;
    float sqr_dist_limit = pow(height/2.f, 2.f) + sqr_radius ;
    if (ray.squaredDistance(center) > sqr_dist_limit)
        return false;

    // The line intersects the bounding sphere, look for intersections with
    // bases of the cylinder.

    size_t found = 0; // counts how many intersections were found
    Eigen::Hyperplane<float, 3> base;
    if (! is_approx(ray.direction().dot(normal), 0.f)) {
        for (size_t i=1; i<=1; --i) {
            Vec3f cylinder_center = pos+i*height*normal;
            if (i == 0) {
                // The hole base can be identical to mesh surface if it is flat
                // let's better move the base outward a bit
                cylinder_center -= EPSILON*normal;
            }
            base = Eigen::Hyperplane<float, 3>(normal, cylinder_center);
            Vec3f intersection = ray.intersectionPoint(base);
            // Only accept the point if it is inside the cylinder base.
            if ((cylinder_center-intersection).squaredNorm() < sqr_radius) {
                out[found].first = ray.intersectionParameter(base);
                out[found].second = (i==0 ? 1. : -1.) * normal.cast<double>();
                ++found;
            }
        }
    }
    else
    {
        // In case the line was perpendicular to the cylinder axis, previous
        // block was skipped, but base will later be assumed to be valid.
        base = Eigen::Hyperplane<float, 3>(normal, pos-EPSILON*normal);
    }

    // In case there is still an intersection to be found, check the wall
    if (found != 2 && ! is_approx(std::abs(ray.direction().dot(normal)), 1.f)) {
        // Project the ray onto the base plane
        Vec3f proj_origin = base.projection(ray.origin());
        Vec3f proj_dir = base.projection(ray.origin()+ray.direction())-proj_origin;
        // save how the parameter scales and normalize the projected direction
        float par_scale = proj_dir.norm();
        proj_dir = proj_dir/par_scale;
        Eigen::ParametrizedLine<float, 3> projected_ray(proj_origin, proj_dir);
        // Calculate point on the secant that's closest to the center
        // and its distance to the circle along the projected line
        Vec3f closest = projected_ray.projection(pos);
        float dist = sqrt((sqr_radius - (closest-pos).squaredNorm()));
        // Unproject both intersections on the original line and check
        // they are on the cylinder and not past it:
        for (int i=-1; i<=1 && found !=2; i+=2) {
            Vec3f isect = closest + i*dist * projected_ray.direction();
            Vec3f to_isect = isect-proj_origin;
            float par = to_isect.norm() / par_scale;
            if (to_isect.normalized().dot(proj_dir.normalized()) < 0.f)
                par *= -1.f;
            Vec3d hit_normal = (pos-isect).normalized().cast<double>();
            isect = ray.pointAt(par);
            // check that the intersection is between the base planes:
            float vert_dist = base.signedDistance(isect);
            if (vert_dist > 0.f && vert_dist < height) {
                out[found].first = par;
                out[found].second = hit_normal;
                ++found;
            }
        }
    }

    // If only one intersection was found, it is some corner case,
    // no intersection will be returned:
    if (found != 2)
        return false;

    // Sort the intersections:
    if (out[0].first > out[1].first)
        std::swap(out[0], out[1]);

    return true;
}

void cut_drainholes(std::vector<ExPolygons> & obj_slices,
                    const std::vector<float> &slicegrid,
                    float                     closing_radius,
                    const sla::DrainHoles &   holes,
                    std::function<void(void)> thr)
{
    TriangleMesh mesh;
    for (const sla::DrainHole &holept : holes)
        mesh.merge(TriangleMesh{holept.to_mesh()});
    
    if (mesh.empty()) return;
    
    std::vector<ExPolygons> hole_slices = slice_mesh_ex(mesh.its, slicegrid, closing_radius, thr);
    
    if (obj_slices.size() != hole_slices.size())
        BOOST_LOG_TRIVIAL(warning)
            << "Sliced object and drain-holes layer count does not match!";

    size_t until = std::min(obj_slices.size(), hole_slices.size());
    
    for (size_t i = 0; i < until; ++i)
        obj_slices[i] = diff_ex(obj_slices[i], hole_slices[i]);
}

void hollow_mesh(TriangleMesh &mesh, const HollowingConfig &cfg, int flags)
{
    InteriorPtr interior = generate_interior(mesh, cfg, JobController{});
    if (!interior) return;

    hollow_mesh(mesh, *interior, flags);
}

void hollow_mesh(TriangleMesh &mesh, const Interior &interior, int flags)
{
    if (mesh.empty() || interior.mesh.empty()) return;

    if (flags & hfRemoveInsideTriangles && interior.gridptr)
        remove_inside_triangles(mesh, interior);

    mesh.merge(TriangleMesh{interior.mesh});
}

// Get the distance of p to the interior's zero iso_surface. Interior should
// have its zero isosurface positioned at offset + closing_distance inwards form
// the model surface.
static double get_distance_raw(const Vec3f &p, const Interior &interior)
{
    assert(interior.gridptr);

    if (!interior.accessor) interior.reset_accessor();

    auto v       = (p * interior.voxel_scale).cast<double>();
    auto grididx = interior.gridptr->transform().worldToIndexCellCentered(
        {v.x(), v.y(), v.z()});

    return interior.accessor->getValue(grididx) ;
}

struct TriangleBubble { Vec3f center; double R; };

// Return the distance of bubble center to the interior boundary or NaN if the
// triangle is too big to be measured.
static double get_distance(const TriangleBubble &b, const Interior &interior)
{
    double R = b.R * interior.voxel_scale;
    double D = get_distance_raw(b.center, interior);

    return (D > 0. && R >= interior.nb_out) ||
           (D < 0. && R >= interior.nb_in)  ||
           ((D - R) < 0. && 2 * R > interior.thickness) ?
                std::nan("") :
                // FIXME: Adding interior.voxel_scale is a compromise supposed
                // to prevent the deletion of the triangles forming the interior
                // itself. This has a side effect that a small portion of the
                // bad triangles will still be visible.
                D - interior.closing_distance /*+ 2 * interior.voxel_scale*/;
}

double get_distance(const Vec3f &p, const Interior &interior)
{
    double d = get_distance_raw(p, interior) - interior.closing_distance;
    return d / interior.voxel_scale;
}

// A face that can be divided. Stores the indices into the original mesh if its
// part of that mesh and the vertices it consists of.
enum { NEW_FACE = -1};
struct DivFace {
    Vec3i32 indx;
    std::array<Vec3f, 3> verts;
    long faceid = NEW_FACE;
    long parent = NEW_FACE;
};

// Divide a face recursively and call visitor on all the sub-faces.
template<class Fn>
void divide_triangle(const DivFace &face, Fn &&visitor)
{
    std::array<Vec3f, 3> edges = {(face.verts[0] - face.verts[1]),
                                  (face.verts[1] - face.verts[2]),
                                  (face.verts[2] - face.verts[0])};

    std::array<size_t, 3> edgeidx = {0, 1, 2};

    std::sort(edgeidx.begin(), edgeidx.end(), [&edges](size_t e1, size_t e2) {
        return edges[e1].squaredNorm() > edges[e2].squaredNorm();
    });

    DivFace child1, child2;

    child1.parent   = face.faceid == NEW_FACE ? face.parent : face.faceid;
    child1.indx(0)  = -1;
    child1.indx(1)  = face.indx(edgeidx[1]);
    child1.indx(2)  = face.indx((edgeidx[1] + 1) % 3);
    child1.verts[0] = (face.verts[edgeidx[0]] + face.verts[(edgeidx[0] + 1) % 3]) / 2.;
    child1.verts[1] = face.verts[edgeidx[1]];
    child1.verts[2] = face.verts[(edgeidx[1] + 1) % 3];

    if (visitor(child1))
        divide_triangle(child1, std::forward<Fn>(visitor));

    child2.parent   = face.faceid == NEW_FACE ? face.parent : face.faceid;
    child2.indx(0)  = -1;
    child2.indx(1)  = face.indx(edgeidx[2]);
    child2.indx(2)  = face.indx((edgeidx[2] + 1) % 3);
    child2.verts[0] = child1.verts[0];
    child2.verts[1] = face.verts[edgeidx[2]];
    child2.verts[2] = face.verts[(edgeidx[2] + 1) % 3];

    if (visitor(child2))
        divide_triangle(child2, std::forward<Fn>(visitor));
}

void remove_inside_triangles(TriangleMesh &mesh, const Interior &interior,
                             const std::vector<bool> &exclude_mask)
{
    enum TrPos { posInside, posTouch, posOutside };

    auto &faces       = mesh.its.indices;
    auto &vertices    = mesh.its.vertices;
    auto bb           = mesh.bounding_box();

    bool use_exclude_mask = faces.size() == exclude_mask.size();
    auto is_excluded = [&exclude_mask, use_exclude_mask](size_t face_id) {
        return use_exclude_mask && exclude_mask[face_id];
    };

    // TODO: Parallel mode not working yet
    using exec_policy = ccr_seq;

    // Info about the needed modifications on the input mesh.
    struct MeshMods {

        // Just a thread safe wrapper for a vector of triangles.
        struct {
            std::vector<std::array<Vec3f, 3>> data;
            exec_policy::SpinningMutex        mutex;

            void emplace_back(const std::array<Vec3f, 3> &pts)
            {
                std::lock_guard lk{mutex};
                data.emplace_back(pts);
            }

            size_t size() const { return data.size(); }
            const std::array<Vec3f, 3>& operator[](size_t idx) const
            {
                return data[idx];
            }

        } new_triangles;

        // A vector of bool for all faces signaling if it needs to be removed
        // or not.
        std::vector<bool> to_remove;

        MeshMods(const TriangleMesh &mesh):
            to_remove(mesh.its.indices.size(), false) {}

        // Number of triangles that need to be removed.
        size_t to_remove_cnt() const
        {
            return std::accumulate(to_remove.begin(), to_remove.end(), size_t(0));
        }

    } mesh_mods{mesh};

    // Must return true if further division of the face is needed.
    auto divfn = [&interior, bb, &mesh_mods](const DivFace &f) {
        BoundingBoxf3 facebb { f.verts.begin(), f.verts.end() };

        // Face is certainly outside the cavity
        if (! facebb.intersects(bb) && f.faceid != NEW_FACE) {
            return false;
        }

        TriangleBubble bubble{facebb.center().cast<float>(), facebb.radius()};

        double D = get_distance(bubble, interior);
        double R = bubble.R * interior.voxel_scale;

        if (std::isnan(D)) // The distance cannot be measured, triangle too big
            return true;

        // Distance of the bubble wall to the interior wall. Negative if the
        // bubble is overlapping with the interior
        double bubble_distance = D - R;

        // The face is crossing the interior or inside, it must be removed and
        // parts of it re-added, that are outside the interior
        if (bubble_distance < 0.) {
            if (f.faceid != NEW_FACE)
                mesh_mods.to_remove[f.faceid] = true;

            if (f.parent != NEW_FACE) // Top parent needs to be removed as well
                mesh_mods.to_remove[f.parent] = true;

            // If the outside part is between the interior end the exterior
            // (inside the wall being invisible), no further division is needed.
            if ((R + D) < interior.thickness)
                return false;

            return true;
        } else if (f.faceid == NEW_FACE) {
            // New face completely outside needs to be re-added.
            mesh_mods.new_triangles.emplace_back(f.verts);
        }

        return false;
    };

    interior.reset_accessor();

    exec_policy::for_each(size_t(0), faces.size(), [&] (size_t face_idx) {
        const Vec3i32 &face = faces[face_idx];

        // If the triangle is excluded, we need to keep it.
        if (is_excluded(face_idx))
            return;

        std::array<Vec3f, 3> pts =
            { vertices[face(0)], vertices[face(1)], vertices[face(2)] };

        BoundingBoxf3 facebb { pts.begin(), pts.end() };

        // Face is certainly outside the cavity
        if (! facebb.intersects(bb)) return;

        DivFace df{face, pts, long(face_idx)};

        if (divfn(df))
            divide_triangle(df, divfn);

    }, exec_policy::max_concurreny());

    auto new_faces = reserve_vector<Vec3i32>(faces.size() +
                                           mesh_mods.new_triangles.size());

    for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
        if (!mesh_mods.to_remove[face_idx])
            new_faces.emplace_back(faces[face_idx]);
    }

    for(size_t i = 0; i < mesh_mods.new_triangles.size(); ++i) {
        size_t o = vertices.size();
        vertices.emplace_back(mesh_mods.new_triangles[i][0]);
        vertices.emplace_back(mesh_mods.new_triangles[i][1]);
        vertices.emplace_back(mesh_mods.new_triangles[i][2]);
        new_faces.emplace_back(int(o), int(o + 1), int(o + 2));
    }

    BOOST_LOG_TRIVIAL(info)
            << "Trimming: " << mesh_mods.to_remove_cnt() << " triangles removed";
    BOOST_LOG_TRIVIAL(info)
            << "Trimming: " << mesh_mods.new_triangles.size() << " triangles added";

    faces.swap(new_faces);
    new_faces = {};

    mesh = TriangleMesh{mesh.its};
    //FIXME do we want to repair the mesh? Are there duplicate vertices or flipped triangles?
}

}} // namespace Slic3r::sla
