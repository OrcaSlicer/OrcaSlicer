#ifndef Slic3r_Measure_hpp_
#define Slic3r_Measure_hpp_

#include <optional>
#include <memory>

#include "Point.hpp"

struct indexed_triangle_set;

namespace Slic3r {

class TriangleMesh;

namespace Measure {
enum class SurfaceFeatureType : int {
    Undef  = 0,
    Point  = 1 << 0,
    Edge   = 1 << 1,
    Circle = 1 << 2,
    Plane  = 1 << 3
};

bool get_point_projection_to_plane(const Vec3d &pt, const Vec3d &plane_origin, const Vec3d &plane_normal, Vec3d &intersection_pt);
Vec3d get_one_point_in_plane(const Vec3d &plane_origin, const Vec3d &plane_normal);

class SurfaceFeature
{
public:
    SurfaceFeature(SurfaceFeatureType type, const Vec3d& pt1, const Vec3d& pt2, std::optional<Vec3d> pt3 = std::nullopt, double value = 0.0)
        : m_type(type), m_pt1(pt1), m_pt2(pt2), m_pt3(pt3), m_value(value) {}

    SurfaceFeature(const Vec3d& pt)
    : m_type{SurfaceFeatureType::Point}, m_pt1{pt} {}

    SurfaceFeature(const SurfaceFeature& sf){
        this->clone(sf);
        volume                 = sf.volume;
        plane_indices          = sf.plane_indices;
        world_tran             = sf.world_tran;
        world_plane_features   = sf.world_plane_features;
        origin_surface_feature = sf.origin_surface_feature;
    }

    void clone(const SurfaceFeature &sf)
    {
        m_type               = sf.get_type();
        m_pt1                = sf.get_pt1();
        m_pt2                = sf.get_pt2();
        m_pt3                = sf.get_pt3();
        m_value              = sf.get_value();
    }
    void translate(const Vec3d& displacement);
    void translate(const Transform3d& tran);
    // Get type of this feature.
    SurfaceFeatureType get_type() const { return m_type; }

    // For points, return the point.
    Vec3d get_point() const { assert(m_type == SurfaceFeatureType::Point); return m_pt1; }
    // For edges, return start and end.
    std::pair<Vec3d, Vec3d> get_edge() const { assert(m_type == SurfaceFeatureType::Edge); return std::make_pair(m_pt1, m_pt2); }

    // For circles, return center, radius and normal.
    std::tuple<Vec3d, double, Vec3d> get_circle() const { assert(m_type == SurfaceFeatureType::Circle); return std::make_tuple(m_pt1, m_value, m_pt2); }

    // For planes, return index into vector provided by Measuring::get_plane_triangle_indices, normal and point.
    std::tuple<int, Vec3d, Vec3d> get_plane() const { assert(m_type == SurfaceFeatureType::Plane); return std::make_tuple(int(m_value), m_pt1, m_pt2); }

    // For anything, return an extra point that should also be considered a part of this.
    std::optional<Vec3d> get_extra_point() const { assert(m_type != SurfaceFeatureType::Undef); return m_pt3; }

    bool operator == (const SurfaceFeature& other) const {
        if (this->m_type != other.m_type) return false;
        switch (this->m_type)
        {
        case SurfaceFeatureType::Undef: { break; }
        case SurfaceFeatureType::Point: { return (this->m_pt1.isApprox(other.m_pt1)); }
        case SurfaceFeatureType::Edge: {
            return (this->m_pt1.isApprox(other.m_pt1) && this->m_pt2.isApprox(other.m_pt2)) ||
                   (this->m_pt1.isApprox(other.m_pt2) && this->m_pt2.isApprox(other.m_pt1));
        }
        case SurfaceFeatureType::Plane:
        case SurfaceFeatureType::Circle: {
            return (this->m_pt1.isApprox(other.m_pt1) && this->m_pt2.isApprox(other.m_pt2) && std::abs(this->m_value - other.m_value) < EPSILON);
        }
        }

        return false;
    }

    bool operator != (const SurfaceFeature& other) const {
        return !operator == (other);
    }

    void* volume{nullptr};
    std::vector<int>*    plane_indices{nullptr};
    Transform3d                  world_tran { Transform3d::Identity() };
    std::shared_ptr<std::vector<SurfaceFeature>> world_plane_features{nullptr};
    std::shared_ptr<SurfaceFeature> origin_surface_feature{nullptr};

    Vec3d get_pt1() const{ return m_pt1; }
    Vec3d                       get_pt2() const { return m_pt2; }
    const std::optional<Vec3d>& get_pt3() const { return m_pt3; }
    double                      get_value() const { return m_value; }

private:
    SurfaceFeatureType m_type{ SurfaceFeatureType::Undef };
    Vec3d m_pt1{ Vec3d::Zero() };
    Vec3d m_pt2{ Vec3d::Zero() };
    std::optional<Vec3d> m_pt3;
    double m_value{ 0.0 };
};



class MeasuringImpl;


class Measuring {
public:
    // Construct the measurement object on a given its.
    explicit Measuring(const indexed_triangle_set& its);
    ~Measuring();


    // Given a face_idx where the mouse cursor points, return a feature that
    // should be highlighted (if any).
    std::optional<SurfaceFeature> get_feature(size_t face_idx, const Vec3d& point, const Transform3d & world_tran,bool only_select_plane) const;

    // Return total number of planes.
    int get_num_of_planes() const;

    // Returns a list of triangle indices for given plane.
    const std::vector<int>& get_plane_triangle_indices(int idx) const;

    // Returns the surface features of the plane with the given index
    const std::vector<SurfaceFeature>& get_plane_features(unsigned int plane_id) const;

    // Returns the mesh used for measuring
    const indexed_triangle_set& get_its() const;

private:
    std::unique_ptr<MeasuringImpl> priv;
};


struct DistAndPoints {
    DistAndPoints(double dist_, Vec3d from_, Vec3d to_) : dist(dist_), from(from_), to(to_) {}
    double dist;
    Vec3d from;
    Vec3d to;
};

struct AngleAndEdges {
    AngleAndEdges(double angle_, const Vec3d& center_, const std::pair<Vec3d, Vec3d>& e1_, const std::pair<Vec3d, Vec3d>& e2_, double radius_, bool coplanar_)
        : angle(angle_), center(center_), e1(e1_), e2(e2_), radius(radius_), coplanar(coplanar_) {}
    double angle;
    Vec3d center;
    std::pair<Vec3d, Vec3d> e1;
    std::pair<Vec3d, Vec3d> e2;
    double radius;
    bool coplanar;

    static const AngleAndEdges Dummy;
};

struct MeasurementResult {
    std::optional<AngleAndEdges> angle;
    std::optional<DistAndPoints> distance_infinite;
    std::optional<DistAndPoints> distance_strict;
    std::optional<Vec3d>  distance_xyz;

    bool has_distance_data() const {
        return distance_infinite.has_value() || distance_strict.has_value();
    }

    bool has_any_data() const {
        return angle.has_value() || distance_infinite.has_value() || distance_strict.has_value() || distance_xyz.has_value();
    }
};

// Returns distance/angle between two SurfaceFeatures.
MeasurementResult get_measurement(const SurfaceFeature& a, const SurfaceFeature& b,bool deal_circle_result =false);
bool              can_set_xyz_distance(const SurfaceFeature &a, const SurfaceFeature &b);

struct AssemblyAction
{
    bool                         can_set_to_parallel{false};
    bool                         can_set_to_center_coincidence{false};
    bool                         can_set_feature_1_reverse_rotation{false};
    bool                         can_set_feature_2_reverse_rotation{false};
    bool                         can_around_center_of_faces{false};
    bool                         has_parallel_distance{false};
    float                        parallel_distance;
    float                        angle_radian{0};
    Transform3d                  tran_for_parallel;
    Transform3d                  tran_for_center_coincidence;
    Transform3d                  tran_for_reverse_rotation;
};
AssemblyAction get_assembly_action(const SurfaceFeature &a, const SurfaceFeature &b);

inline Vec3d edge_direction(const Vec3d& from, const Vec3d& to) { return (to - from).normalized(); }
inline Vec3d edge_direction(const std::pair<Vec3d, Vec3d>& e) { return edge_direction(e.first, e.second); }
inline Vec3d edge_direction(const SurfaceFeature& edge) {
    assert(edge.get_type() == SurfaceFeatureType::Edge);
    return edge_direction(edge.get_edge());
}

inline Vec3d plane_normal(const SurfaceFeature& plane) {
    assert(plane.get_type() == SurfaceFeatureType::Plane);
    return std::get<1>(plane.get_plane());
}

inline bool are_parallel(const Vec3d& v1, const Vec3d& v2) { return std::abs(std::abs(v1.dot(v2)) - 1.0) < EPSILON; }
inline bool are_perpendicular(const Vec3d& v1, const Vec3d& v2) { return std::abs(v1.dot(v2)) < EPSILON; }

inline bool are_parallel(const std::pair<Vec3d, Vec3d>& e1, const std::pair<Vec3d, Vec3d>& e2) {
    return are_parallel(e1.second - e1.first, e2.second - e2.first);
}
inline bool are_parallel(const SurfaceFeature& f1, const SurfaceFeature& f2) {
    if (f1.get_type() == SurfaceFeatureType::Edge && f2.get_type() == SurfaceFeatureType::Edge)
        return are_parallel(edge_direction(f1), edge_direction(f2));
    else if (f1.get_type() == SurfaceFeatureType::Edge && f2.get_type() == SurfaceFeatureType::Plane)
        return are_perpendicular(edge_direction(f1), plane_normal(f2));
    else
        return false;
}

inline bool are_perpendicular(const SurfaceFeature& f1, const SurfaceFeature& f2) {
    if (f1.get_type() == SurfaceFeatureType::Edge && f2.get_type() == SurfaceFeatureType::Edge)
        return are_perpendicular(edge_direction(f1), edge_direction(f2));
    else if (f1.get_type() == SurfaceFeatureType::Edge && f2.get_type() == SurfaceFeatureType::Plane)
        return are_parallel(edge_direction(f1), plane_normal(f2));
    else
        return false;
}

} // namespace Measure
} // namespace Slic3r

#endif // Slic3r_Measure_hpp_
