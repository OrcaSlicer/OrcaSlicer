#ifndef SLA_INDEXEDMESH_H
#define SLA_INDEXEDMESH_H

#include <memory>
#include <vector>

#include <libslic3r/Point.hpp>

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

namespace sla {

using PointSet = Eigen::MatrixXd;

/// An index-triangle structure for libIGL functions. Also serves as an
/// alternative (raw) input format for the SLASupportTree.
//  Implemented in libslic3r/SLA/Common.cpp
class IndexedMesh {
    class AABBImpl;
    
    const indexed_triangle_set* m_tm;
    double m_ground_level = 0, m_gnd_offset = 0;
    
    std::unique_ptr<AABBImpl> m_aabb;

#ifdef SLIC3R_HOLE_RAYCASTER
    // This holds a copy of holes in the mesh. Initialized externally
    // by load_mesh setter.
    std::vector<DrainHole> m_holes;
#endif

    template<class M> void init(const M &mesh, bool calculate_epsilon);

public:
    
    // calculate_epsilon ... calculate epsilon for triangle-ray intersection from an average triangle edge length.
    // If set to false, a default epsilon is used, which works for "reasonable" meshes.
    explicit IndexedMesh(const indexed_triangle_set &tmesh, bool calculate_epsilon = false);
    explicit IndexedMesh(const TriangleMesh &mesh, bool calculate_epsilon = false);
    
    IndexedMesh(const IndexedMesh& other);
    IndexedMesh& operator=(const IndexedMesh&);
    
    IndexedMesh(IndexedMesh &&other);
    IndexedMesh& operator=(IndexedMesh &&other);
    
    ~IndexedMesh();
    
    inline double ground_level() const { return m_ground_level + m_gnd_offset; }
    inline void ground_level_offset(double o) { m_gnd_offset = o; }
    inline double ground_level_offset() const { return m_gnd_offset; }
    
    const std::vector<Vec3f>& vertices() const;
    const std::vector<Vec3i32>& indices()  const;
    const Vec3f& vertices(size_t idx) const;
    const Vec3i32& indices(size_t idx) const;
    
    // Result of a raycast
    class hit_result {
        // m_t holds a distance from m_source to the intersection.
        double m_t = infty();
        int m_face_id = -1;
        const IndexedMesh *m_mesh = nullptr;
        Vec3d m_dir;
        Vec3d m_source;
        Vec3d m_normal;
        friend class IndexedMesh;
        
        // A valid object of this class can only be obtained from
        // IndexedMesh::query_ray_hit method.
        explicit inline hit_result(const IndexedMesh& em): m_mesh(&em) {}
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
    void load_holes(const std::vector<DrainHole>& holes) {
        m_holes = holes;
    }

    // Iterates over hits and holes and returns the true hit, possibly
    // on the inside of a hole.
    // This function is currently not used anywhere, it was written when the
    // holes were subtracted on slices, that is, before we started using CGAL
    // to actually cut the holes into the mesh.
    hit_result filter_hits(const std::vector<IndexedMesh::hit_result>& obj_hits) const;
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
};

// Calculate the normals for the selected points (from 'points' set) on the
// mesh. This will call squared distance for each point.
PointSet normals(const PointSet& points,
    const IndexedMesh& convert_mesh,
    double eps = 0.05,  // min distance from edges
    std::function<void()> throw_on_cancel = [](){},
    const std::vector<unsigned>& selected_points = {});

}} // namespace Slic3r::sla

#endif // INDEXEDMESH_H
