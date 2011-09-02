#include "IndexedMesh.hpp"
#include "Concurrency.hpp"

#include <libslic3r/AABBTreeIndirect.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <numeric>

#ifdef SLIC3R_HOLE_RAYCASTER
#include <libslic3r/SLA/Hollowing.hpp>
#endif

namespace Slic3r {

namespace sla {

class IndexedMesh::AABBImpl {
private:
    AABBTreeIndirect::Tree3f m_tree;
    double                   m_triangle_ray_epsilon;

public:
    void init(const indexed_triangle_set &its, bool calculate_epsilon)
    {
        m_triangle_ray_epsilon = 0.000001;
        if (calculate_epsilon) {
            // Calculate epsilon from average triangle edge length.
            double l = its_average_edge_length(its);
            if (l > 0)
                m_triangle_ray_epsilon = 0.000001 * l * l;
        }
        m_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(
            its.vertices, its.indices);
    }

    void intersect_ray(const indexed_triangle_set &its,
                       const Vec3d &               s,
                       const Vec3d &               dir,
                       igl::Hit &                  hit)
    {
        AABBTreeIndirect::intersect_ray_first_hit(its.vertices, its.indices,
                                                  m_tree, s, dir, hit, m_triangle_ray_epsilon);
    }

    void intersect_ray(const indexed_triangle_set &its,
                       const Vec3d &               s,
                       const Vec3d &               dir,
                       std::vector<igl::Hit> &     hits)
    {
        AABBTreeIndirect::intersect_ray_all_hits(its.vertices, its.indices,
                                                 m_tree, s, dir, hits, m_triangle_ray_epsilon);
    }

    double squared_distance(const indexed_triangle_set & its,
                            const Vec3d &                point,
                            int &                        i,
                            Eigen::Matrix<double, 1, 3> &closest)
    {
        size_t idx_unsigned = 0;
        Vec3d  closest_vec3d(closest);
        double dist =
            AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                its.vertices, its.indices, m_tree, point, idx_unsigned,
                closest_vec3d);
        i       = int(idx_unsigned);
        closest = closest_vec3d;
        return dist;
    }
};

template<class M> void IndexedMesh::init(const M &mesh, bool calculate_epsilon)
{
    BoundingBoxf3 bb = bounding_box(mesh);
    m_ground_level += bb.min(Z);

    // Build the AABB accelaration tree
    m_aabb->init(*m_tm, calculate_epsilon);
}

IndexedMesh::IndexedMesh(const indexed_triangle_set& tmesh, bool calculate_epsilon)
    : m_aabb(new AABBImpl()), m_tm(&tmesh)
{
    init(tmesh, calculate_epsilon);
}

IndexedMesh::IndexedMesh(const TriangleMesh &mesh, bool calculate_epsilon)
    : m_aabb(new AABBImpl()), m_tm(&mesh.its)
{
    init(mesh, calculate_epsilon);
}

IndexedMesh::~IndexedMesh() {}

IndexedMesh::IndexedMesh(const IndexedMesh &other):
    m_tm(other.m_tm), m_ground_level(other.m_ground_level),
    m_aabb( new AABBImpl(*other.m_aabb) ) {}


IndexedMesh &IndexedMesh::operator=(const IndexedMesh &other)
{
    m_tm = other.m_tm;
    m_ground_level = other.m_ground_level;
    m_aabb.reset(new AABBImpl(*other.m_aabb)); return *this;
}

IndexedMesh &IndexedMesh::operator=(IndexedMesh &&other) = default;

IndexedMesh::IndexedMesh(IndexedMesh &&other) = default;



const std::vector<Vec3f>& IndexedMesh::vertices() const
{
    return m_tm->vertices;
}



const std::vector<Vec3i32>& IndexedMesh::indices()  const
{
    return m_tm->indices;
}



const Vec3f& IndexedMesh::vertices(size_t idx) const
{
    return m_tm->vertices[idx];
}



const Vec3i32& IndexedMesh::indices(size_t idx) const
{
    return m_tm->indices[idx];
}


Vec3d IndexedMesh::normal_by_face_id(int face_id) const {

    return its_unnormalized_normal(*m_tm, face_id).cast<double>().normalized();
}


IndexedMesh::hit_result
IndexedMesh::query_ray_hit(const Vec3d &s, const Vec3d &dir) const
{
    assert(is_approx(dir.norm(), 1.));
    igl::Hit hit{-1, -1, 0.f, 0.f, 0.f};
    hit.t = std::numeric_limits<float>::infinity();

#ifdef SLIC3R_HOLE_RAYCASTER
    if (! m_holes.empty()) {

        // If there are holes, the hit_results will be made by
        // query_ray_hits (object) and filter_hits (holes):
        return filter_hits(query_ray_hits(s, dir));
    }
#endif

    m_aabb->intersect_ray(*m_tm, s, dir, hit);
    hit_result ret(*this);
    ret.m_t = double(hit.t);
    ret.m_dir = dir;
    ret.m_source = s;
    if(!std::isinf(hit.t) && !std::isnan(hit.t)) {
        ret.m_normal = this->normal_by_face_id(hit.id);
        ret.m_face_id = hit.id;
    }

    return ret;
}

std::vector<IndexedMesh::hit_result>
IndexedMesh::query_ray_hits(const Vec3d &s, const Vec3d &dir) const
{
    std::vector<IndexedMesh::hit_result> outs;
    std::vector<igl::Hit> hits;
    m_aabb->intersect_ray(*m_tm, s, dir, hits);

    // The sort is necessary, the hits are not always sorted.
    std::sort(hits.begin(), hits.end(),
              [](const igl::Hit& a, const igl::Hit& b) { return a.t < b.t; });

    // Remove duplicates. They sometimes appear, for example when the ray is cast
    // along an axis of a cube due to floating-point approximations in igl (?)
    // BBS: STUDIO-2591 A mesh with overlapping faces cannot be painted
    //hits.erase(std::unique(hits.begin(), hits.end(),
    //                       [](const igl::Hit& a, const igl::Hit& b)
    //                       { return a.t == b.t; }),
    //           hits.end());

    //  Convert the igl::Hit into hit_result
    outs.reserve(hits.size());
    for (const igl::Hit& hit : hits) {
        outs.emplace_back(IndexedMesh::hit_result(*this));
        outs.back().m_t = double(hit.t);
        outs.back().m_dir = dir;
        outs.back().m_source = s;
        if(!std::isinf(hit.t) && !std::isnan(hit.t)) {
            outs.back().m_normal = this->normal_by_face_id(hit.id);
            outs.back().m_face_id = hit.id;
        }
    }

    return outs;
}


#ifdef SLIC3R_HOLE_RAYCASTER
IndexedMesh::hit_result IndexedMesh::filter_hits(
    const std::vector<IndexedMesh::hit_result>& object_hits) const
{
    assert(! m_holes.empty());
    hit_result out(*this);

    if (object_hits.empty())
        return out;

    const Vec3d& s = object_hits.front().source();
    const Vec3d& dir = object_hits.front().direction();

    // A helper struct to save an intersetion with a hole
    struct HoleHit {
        HoleHit(float t_p, const Vec3d& normal_p, bool entry_p) :
            t(t_p), normal(normal_p), entry(entry_p) {}
        float t;
        Vec3d normal;
        bool entry;
    };
    std::vector<HoleHit> hole_isects;
    hole_isects.reserve(m_holes.size());

    auto sf = s.cast<float>();
    auto dirf = dir.cast<float>();

    // Collect hits on all holes, preserve information about entry/exit
    for (const sla::DrainHole& hole : m_holes) {
        std::array<std::pair<float, Vec3d>, 2> isects;
        if (hole.get_intersections(sf, dirf, isects)) {
            // Ignore hole hits behind the source
            if (isects[0].first > 0.f) hole_isects.emplace_back(isects[0].first, isects[0].second, true);
            if (isects[1].first > 0.f) hole_isects.emplace_back(isects[1].first, isects[1].second, false);
        }
    }

    // Holes can intersect each other, sort the hits by t
    std::sort(hole_isects.begin(), hole_isects.end(),
              [](const HoleHit& a, const HoleHit& b) { return a.t < b.t; });

    // Now inspect the intersections with object and holes, in the order of
    // increasing distance. Keep track how deep are we nested in mesh/holes and
    // pick the correct intersection.
    // This needs to be done twice - first to find out how deep in the structure
    // the source is, then to pick the correct intersection.
    int hole_nested = 0;
    int object_nested = 0;
    for (int dry_run=1; dry_run>=0; --dry_run) {
        hole_nested = -hole_nested;
        object_nested = -object_nested;

        bool is_hole = false;
        bool is_entry = false;
        const HoleHit* next_hole_hit = hole_isects.empty() ? nullptr : &hole_isects.front();
        const hit_result* next_mesh_hit = &object_hits.front();

        while (next_hole_hit || next_mesh_hit) {
            if (next_hole_hit && next_mesh_hit) // still have hole and obj hits
                is_hole = (next_hole_hit->t < next_mesh_hit->m_t);
            else
                is_hole = next_hole_hit; // one or the other ran out

            // Is this entry or exit hit?
            is_entry = is_hole ? next_hole_hit->entry : ! next_mesh_hit->is_inside();

            if (! dry_run) {
                if (! is_hole && hole_nested == 0) {
                    // This is a valid object hit
                    return *next_mesh_hit;
                }
                if (is_hole && ! is_entry && object_nested != 0) {
                    // This holehit is the one we seek
                    out.m_t = next_hole_hit->t;
                    out.m_normal = next_hole_hit->normal;
                    out.m_source = s;
                    out.m_dir = dir;
                    return out;
                }
            }

            // Increase/decrease the counter
            (is_hole ? hole_nested : object_nested) += (is_entry ? 1 : -1);

            // Advance the respective pointer
            if (is_hole && next_hole_hit++ == &hole_isects.back())
                next_hole_hit = nullptr;
            if (! is_hole && next_mesh_hit++ == &object_hits.back())
                next_mesh_hit = nullptr;
        }
    }

    // if we got here, the ray ended up in infinity
    return out;
}
#endif


double IndexedMesh::squared_distance(const Vec3d &p, int& i, Vec3d& c) const {
    double sqdst = 0;
    Eigen::Matrix<double, 1, 3> pp = p;
    Eigen::Matrix<double, 1, 3> cc;
    sqdst = m_aabb->squared_distance(*m_tm, pp, i, cc);
    c = cc;
    return sqdst;
}


static bool point_on_edge(const Vec3d& p, const Vec3d& e1, const Vec3d& e2,
                          double eps = 0.05)
{
    using Line3D = Eigen::ParametrizedLine<double, 3>;

    auto line = Line3D::Through(e1, e2);
    double d = line.distance(p);
    return std::abs(d) < eps;
}

PointSet normals(const PointSet& points,
                 const IndexedMesh& mesh,
                 double eps,
                 std::function<void()> thr, // throw on cancel
                 const std::vector<unsigned>& pt_indices)
{
    if (points.rows() == 0 || mesh.vertices().empty() || mesh.indices().empty())
        return {};

    std::vector<unsigned> range = pt_indices;
    if (range.empty()) {
        range.resize(size_t(points.rows()), 0);
        std::iota(range.begin(), range.end(), 0);
    }

    PointSet ret(range.size(), 3);

    //    for (size_t ridx = 0; ridx < range.size(); ++ridx)
    ccr::for_each(size_t(0), range.size(),
        [&ret, &mesh, &points, thr, eps, &range](size_t ridx) {
            thr();
            unsigned el = range[ridx];
            auto  eidx   = Eigen::Index(el);
            int   faceid = 0;
            Vec3d p;

            mesh.squared_distance(points.row(eidx), faceid, p);

            auto trindex = mesh.indices(faceid);

            const Vec3d &p1 = mesh.vertices(trindex(0)).cast<double>();
            const Vec3d &p2 = mesh.vertices(trindex(1)).cast<double>();
            const Vec3d &p3 = mesh.vertices(trindex(2)).cast<double>();

            // We should check if the point lies on an edge of the hosting
            // triangle. If it does then all the other triangles using the
            // same two points have to be searched and the final normal should
            // be some kind of aggregation of the participating triangle
            // normals. We should also consider the cases where the support
            // point lies right on a vertex of its triangle. The procedure is
            // the same, get the neighbor triangles and calculate an average
            // normal.

            // mark the vertex indices of the edge. ia and ib marks and edge
            // ic will mark a single vertex.
            int ia = -1, ib = -1, ic = -1;

            if (std::abs((p - p1).norm()) < eps) {
                ic = trindex(0);
            } else if (std::abs((p - p2).norm()) < eps) {
                ic = trindex(1);
            } else if (std::abs((p - p3).norm()) < eps) {
                ic = trindex(2);
            } else if (point_on_edge(p, p1, p2, eps)) {
                ia = trindex(0);
                ib = trindex(1);
            } else if (point_on_edge(p, p2, p3, eps)) {
                ia = trindex(1);
                ib = trindex(2);
            } else if (point_on_edge(p, p1, p3, eps)) {
                ia = trindex(0);
                ib = trindex(2);
            }

            // vector for the neigboring triangles including the detected one.
            std::vector<size_t> neigh;
            if (ic >= 0) { // The point is right on a vertex of the triangle
                for (size_t n = 0; n < mesh.indices().size(); ++n) {
                    thr();
                    Vec3i32 ni = mesh.indices(n);
                    if ((ni(X) == ic || ni(Y) == ic || ni(Z) == ic))
                        neigh.emplace_back(n);
                }
            } else if (ia >= 0 && ib >= 0) { // the point is on and edge
                // now get all the neigboring triangles
                for (size_t n = 0; n < mesh.indices().size(); ++n) {
                    thr();
                    Vec3i32 ni = mesh.indices(n);
                    if ((ni(X) == ia || ni(Y) == ia || ni(Z) == ia) &&
                        (ni(X) == ib || ni(Y) == ib || ni(Z) == ib))
                        neigh.emplace_back(n);
                }
            }

            // Calculate the normals for the neighboring triangles
            std::vector<Vec3d> neighnorms;
            neighnorms.reserve(neigh.size());
            for (size_t &tri_id : neigh)
                neighnorms.emplace_back(mesh.normal_by_face_id(tri_id));

            // Throw out duplicates. They would cause trouble with summing. We
            // will use std::unique which works on sorted ranges. We will sort
            // by the coefficient-wise sum of the normals. It should force the
            // same elements to be consecutive.
            std::sort(neighnorms.begin(), neighnorms.end(),
                      [](const Vec3d &v1, const Vec3d &v2) {
                          return v1.sum() < v2.sum();
                      });

            auto lend = std::unique(neighnorms.begin(), neighnorms.end(),
                                    [](const Vec3d &n1, const Vec3d &n2) {
                                        // Compare normals for equivalence.
                                        // This is controvers stuff.
                                        auto deq = [](double a, double b) {
                                            return std::abs(a - b) < 1e-3;
                                        };
                                        return deq(n1(X), n2(X)) &&
                                               deq(n1(Y), n2(Y)) &&
                                               deq(n1(Z), n2(Z));
                                    });

            if (!neighnorms.empty()) { // there were neighbors to count with
                // sum up the normals and then normalize the result again.
                // This unification seems to be enough.
                Vec3d sumnorm(0, 0, 0);
                sumnorm = std::accumulate(neighnorms.begin(), lend, sumnorm);
                sumnorm.normalize();
                ret.row(long(ridx)) = sumnorm;
            } else { // point lies safely within its triangle
                Eigen::Vector3d U   = p2 - p1;
                Eigen::Vector3d V   = p3 - p1;
                ret.row(long(ridx)) = U.cross(V).normalized();
            }
        });

    return ret;
}

}} // namespace Slic3r::sla
