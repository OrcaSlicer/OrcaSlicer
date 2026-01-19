#ifndef PRUSASLICER_AABBMESH_H
#define PRUSASLICER_AABBMESH_H

#include <memory>
#include <vector>

#include <libslic3r/Point.hpp>
#include <libslic3r/TriangleMesh.hpp>

// There is an implementation of a hole-aware raycaster that was eventually
// not used in production version. It is now hidden under following define
// for possible future use.
// #define SLIC3R_HOLE_RAYCASTER

#ifdef SLIC3R_HOLE_RAYCASTER
  #include "libslic3r/SLA/Hollowing.hpp"
#endif

struct indexed_triangle_set;

namespace Slic3r {

class TriangleMesh;

// An index-triangle structure coupled with an AABB index to support ray
// casting and other higher level operations.
class AABBMesh {
    class AABBImpl;

    const indexed_triangle_set* m_tm;

    std::unique_ptr<AABBImpl> m_aabb;
    VertexFaceIndex m_vfidx;    // vertex-face index
    std::vector<Vec3i32> m_fnidx; // face-neighbor index

#ifdef SLIC3R_HOLE_RAYCASTER
    // This holds a copy of holes in the mesh. Initialized externally
    // by load_mesh setter.
    std::vector<sla::DrainHole> m_holes;
#endif

    template<class M> void init(const M &mesh, bool calculate_epsilon);

public:

    // calculate_epsilon ... calculate epsilon for triangle-ray intersection from an average triangle edge length.
    // If set to false, a default epsilon is used, which works for "reasonable" meshes.
    explicit AABBMesh(const indexed_triangle_set &tmesh, bool calculate_epsilon = false);
    explicit AABBMesh(const TriangleMesh &mesh, bool calculate_epsilon = false);
    
    AABBMesh(const AABBMesh& other);
    AABBMesh& operator=(const AABBMesh&);

    AABBMesh(AABBMesh &&other);
    AABBMesh& operator=(AABBMesh &&other);

    ~AABBMesh();

    const std::vector<Vec3f>& vertices() const;
    const std::vector<Vec3i32>& indices()  const;
    const Vec3f& vertices(size_t idx) const;
    const Vec3i32& indices(size_t idx) const;

    // Result of a raycast
    class hit_result {
        // m_t holds a distance from m_source to the intersection.
        double m_t = infty();
        int m_face_id = -1;
        const AABBMesh *m_mesh = nullptr;
        Vec3d m_dir    = Vec3d::Zero();
        Vec3d m_source = Vec3d::Zero();
        Vec3d m_normal = Vec3d::Zero();
        friend class AABBMesh;
        
        // A valid object of this class can only be obtained from
        // IndexedMesh::query_ray_hit method.
        explicit inline hit_result(const AABBMesh& em): m_mesh(&em) {}
    public:
        // This denotes no hit on the mesh.
        static inline constexpr double infty() { return std::numeric_limits<double>::infinity(); }
        
        explicit inline hit_result(double val = infty()) : m_t(val) {}
        
        inline double distance() const { return m_t; }
        inline const Vec3d& direction() const { return m_dir; }
        inline const Vec3d& source() const { return m_source; }
        inline Vec3d position() const { return m_source + m_dir * m_t; }
        inline int face() const { return m_face_id; }
        inline bool is_valid() const { return m_mesh != nullptr; }
        inline bool is_hit() const { return m_face_id >= 0 && !std::isinf(m_t); }

        inline const Vec3d& normal() const {
            assert(is_valid());
            return m_normal;
        }

        inline bool is_inside() const {
            return is_hit() && normal().dot(m_dir) > 0;
        }
    };

#ifdef SLIC3R_HOLE_RAYCASTER
    // Inform the object about location of holes
    // creates internal copy of the vector
    void load_holes(const std::vector<sla::DrainHole>& holes) {
        m_holes = holes;
    }

    // Iterates over hits and holes and returns the true hit, possibly
    // on the inside of a hole.
    // This function is currently not used anywhere, it was written when the
    // holes were subtracted on slices, that is, before we started using CGAL
    // to actually cut the holes into the mesh.
    hit_result filter_hits(const std::vector<AABBMesh::hit_result>& obj_hits) const;
#endif

    // Casting a ray on the mesh, returns the distance where the hit occures.
    hit_result query_ray_hit(const Vec3d &s, const Vec3d &dir) const;
    
    // Casts a ray on the mesh and returns all hits
    std::vector<hit_result> query_ray_hits(const Vec3d &s, const Vec3d &dir) const;

    double squared_distance(const Vec3d& p, int& i, Vec3d& c) const;
    inline double squared_distance(const Vec3d &p) const
    {
        int   i;
        Vec3d c;
        return squared_distance(p, i, c);
    }

    Vec3d normal_by_face_id(int face_id) const;

    const indexed_triangle_set * get_triangle_mesh() const { return m_tm; }

    const VertexFaceIndex &vertex_face_index() const { return m_vfidx; }
    const std::vector<Vec3i32> &face_neighbor_index() const { return m_fnidx; }
};


} // namespace Slic3r::sla

#endif // INDEXEDMESH_H
