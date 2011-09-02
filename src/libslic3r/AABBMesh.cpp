#include "AABBMesh.hpp"
#include <Execution/ExecutionTBB.hpp>

#include <libslic3r/AABBTreeIndirect.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <numeric>

#ifdef SLIC3R_HOLE_RAYCASTER
#include <libslic3r/SLA/Hollowing.hpp>
#endif

namespace Slic3r {

class AABBMesh::AABBImpl {
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

template<class M> void AABBMesh::init(const M &mesh, bool calculate_epsilon)
{
    // Build the AABB accelaration tree
    m_aabb->init(*m_tm, calculate_epsilon);
}

AABBMesh::AABBMesh(const indexed_triangle_set &tmesh, bool calculate_epsilon)
    : m_tm(&tmesh)
    , m_aabb(new AABBImpl())
    , m_vfidx{tmesh}
    , m_fnidx{its_face_neighbors(tmesh)}
{
    init(tmesh, calculate_epsilon);
}

AABBMesh::AABBMesh(const TriangleMesh &mesh, bool calculate_epsilon)
    : m_tm(&mesh.its)
    , m_aabb(new AABBImpl())
    , m_vfidx{mesh.its}
    , m_fnidx{its_face_neighbors(mesh.its)}
{
    init(mesh, calculate_epsilon);
}

AABBMesh::~AABBMesh() {}

AABBMesh::AABBMesh(const AABBMesh &other)
    : m_tm(other.m_tm)
    , m_aabb(new AABBImpl(*other.m_aabb))
    , m_vfidx{other.m_vfidx}
    , m_fnidx{other.m_fnidx}
{}

AABBMesh &AABBMesh::operator=(const AABBMesh &other)
{
    m_tm = other.m_tm;
    m_aabb.reset(new AABBImpl(*other.m_aabb));
    m_vfidx = other.m_vfidx;
    m_fnidx = other.m_fnidx;

    return *this;
}

AABBMesh &AABBMesh::operator=(AABBMesh &&other) = default;

AABBMesh::AABBMesh(AABBMesh &&other) = default;



const std::vector<Vec3f>& AABBMesh::vertices() const
{
    return m_tm->vertices;
}



const std::vector<Vec3i32>& AABBMesh::indices()  const
{
    return m_tm->indices;
}



const Vec3f& AABBMesh::vertices(size_t idx) const
{
    return m_tm->vertices[idx];
}



const Vec3i32& AABBMesh::indices(size_t idx) const
{
    return m_tm->indices[idx];
}


Vec3d AABBMesh::normal_by_face_id(int face_id) const {

    return its_unnormalized_normal(*m_tm, face_id).cast<double>().normalized();
}


AABBMesh::hit_result
AABBMesh::query_ray_hit(const Vec3d &s, const Vec3d &dir) const
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

std::vector<AABBMesh::hit_result>
AABBMesh::query_ray_hits(const Vec3d &s, const Vec3d &dir) const
{
    std::vector<AABBMesh::hit_result> outs;
    std::vector<igl::Hit> hits;
    m_aabb->intersect_ray(*m_tm, s, dir, hits);

    // The sort is necessary, the hits are not always sorted.
    std::sort(hits.begin(), hits.end(),
              [](const igl::Hit& a, const igl::Hit& b) { return a.t < b.t; });

    // Remove duplicates. They sometimes appear, for example when the ray is cast
    // along an axis of a cube due to floating-point approximations in igl (?)
    hits.erase(std::unique(hits.begin(), hits.end(),
                           [](const igl::Hit& a, const igl::Hit& b)
                           { return a.t == b.t; }),
               hits.end());

    //  Convert the igl::Hit into hit_result
    outs.reserve(hits.size());
    for (const igl::Hit& hit : hits) {
        outs.emplace_back(AABBMesh::hit_result(*this));
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
AABBMesh::hit_result IndexedMesh::filter_hits(
    const std::vector<AABBMesh::hit_result>& object_hits) const
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


double AABBMesh::squared_distance(const Vec3d &p, int& i, Vec3d& c) const {
    double sqdst = 0;
    Eigen::Matrix<double, 1, 3> pp = p;
    Eigen::Matrix<double, 1, 3> cc;
    sqdst = m_aabb->squared_distance(*m_tm, pp, i, cc);
    c = cc;
    return sqdst;
}

} // namespace Slic3r
