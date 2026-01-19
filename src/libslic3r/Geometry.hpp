#ifndef slic3r_Geometry_hpp_
#define slic3r_Geometry_hpp_

#include "libslic3r.h"
#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"

// Serialization through the Cereal library
#include <cereal/access.hpp>

namespace Slic3r {

namespace Geometry {

// Generic result of an orientation predicate.
enum Orientation
{
    ORIENTATION_CCW = 1,
    ORIENTATION_CW = -1,
    ORIENTATION_COLINEAR = 0
};

// Return orientation of the three points (clockwise, counter-clockwise, colinear)
// The predicate is exact for the coord_t type, using 64bit signed integers for the temporaries.
// which means, the coord_t types must not have some of the topmost bits utilized.
// As the points are limited to 30 bits + signum,
// the temporaries u, v, w are limited to 61 bits + signum,
// and d is limited to 63 bits + signum and we are good.
//note: now coord_t is int64_t, so the algorithm is now adjusted to fallback to double is too big.
static inline Orientation orient(const Point &a, const Point &b, const Point &c) {
    //static_assert(sizeof(coord_t) * 2 == sizeof(int64_t), "orient works with 32 bit coordinates");
    // BOOST_STATIC_ASSERT(sizeof(coord_t) == sizeof(int64_t));
    if (a.x() <= 0xffffffff && b.x() <= 0xffffffff && c.x() <= 0xffffffff &&
        a.y() <= 0xffffffff && b.y() <= 0xffffffff && c.y() <= 0xffffffff) {
        int64_t u = int64_t(b(0)) * int64_t(c(1)) - int64_t(b(1)) * int64_t(c(0));
        int64_t v = int64_t(a(0)) * int64_t(c(1)) - int64_t(a(1)) * int64_t(c(0));
        int64_t w = int64_t(a(0)) * int64_t(b(1)) - int64_t(a(1)) * int64_t(b(0));
        int64_t d = u - v + w;
        return (d > 0) ? ORIENTATION_CCW : ((d == 0) ? ORIENTATION_COLINEAR : ORIENTATION_CW);
    } else {
        double u = double(b(0)) * double(c(1)) - double(b(1)) * double(c(0));
        double v = double(a(0)) * double(c(1)) - double(a(1)) * double(c(0));
        double w = double(a(0)) * double(b(1)) - double(a(1)) * double(b(0));
        double d = u - v + w;
        return (d > 0) ? ORIENTATION_CCW : ((d == 0) ? ORIENTATION_COLINEAR : ORIENTATION_CW);
    }
}

// Return orientation of the polygon by checking orientation of the left bottom corner of the polygon
// using exact arithmetics. The input polygon must not contain duplicate points
// (or at least the left bottom corner point must not have duplicates).
static inline bool is_ccw(const Polygon &poly)
{
    // The polygon shall be at least a triangle.
    assert(poly.points.size() >= 3);
    if (poly.points.size() < 3)
        return true;

    // 1) Find the lowest lexicographical point.
    unsigned int imin = 0;
    for (unsigned int i = 1; i < poly.points.size(); ++ i) {
        const Point &pmin = poly.points[imin];
        const Point &p    = poly.points[i];
        if (p(0) < pmin(0) || (p(0) == pmin(0) && p(1) < pmin(1)))
            imin = i;
    }

    // 2) Detect the orientation of the corner imin.
    size_t iPrev = ((imin == 0) ? poly.points.size() : imin) - 1;
    size_t iNext = ((imin + 1 == poly.points.size()) ? 0 : imin + 1);
    Orientation o = orient(poly.points[iPrev], poly.points[imin], poly.points[iNext]);
    // The lowest bottom point must not be collinear if the polygon does not contain duplicate points
    // or overlapping segments.
    assert(o != ORIENTATION_COLINEAR);
    return o == ORIENTATION_CCW;
}

inline bool ray_ray_intersection(const Vec2d &p1, const Vec2d &v1, const Vec2d &p2, const Vec2d &v2, Vec2d &res)
{
    double denom = v1(0) * v2(1) - v2(0) * v1(1);
    if (std::abs(denom) < EPSILON)
        return false;
    double t = (v2(0) * (p1(1) - p2(1)) - v2(1) * (p1(0) - p2(0))) / denom;
    res(0) = p1(0) + t * v1(0);
    res(1) = p1(1) + t * v1(1);
    return true;
}

inline bool segment_segment_intersection(const Vec2d &p1, const Vec2d &v1, const Vec2d &p2, const Vec2d &v2, Vec2d &res)
{
    double denom = v1(0) * v2(1) - v2(0) * v1(1);
    if (std::abs(denom) < EPSILON)
        // Lines are collinear.
        return false;
    double s12_x = p1(0) - p2(0);
    double s12_y = p1(1) - p2(1);
    double s_numer = v1(0) * s12_y - v1(1) * s12_x;
    bool   denom_is_positive = false;
    if (denom < 0.) {
        denom_is_positive = true;
        denom   = - denom;
        s_numer = - s_numer;
    }
    if (s_numer < 0.)
        // Intersection outside of the 1st segment.
        return false;
    double t_numer = v2(0) * s12_y - v2(1) * s12_x;
    if (! denom_is_positive)
        t_numer = - t_numer;
    if (t_numer < 0. || s_numer > denom || t_numer > denom)
        // Intersection outside of the 1st or 2nd segment.
        return false;
    // Intersection inside both of the segments.
    double t = t_numer / denom;
    res(0) = p1(0) + t * v1(0);
    res(1) = p1(1) + t * v1(1);
    return true;
}

inline bool segments_intersect(
	const Slic3r::Point &ip1, const Slic3r::Point &ip2, 
	const Slic3r::Point &jp1, const Slic3r::Point &jp2)
{    
    //assert(ip1 != ip2);
    //assert(jp1 != jp2);

    auto segments_could_intersect = [](
        const Slic3r::Point &ip1, const Slic3r::Point &ip2,
        const Slic3r::Point &jp1, const Slic3r::Point &jp2) -> std::pair<int, int>
    {
        Vec2i64 iv   = (ip2 - ip1).cast<int64_t>();
        Vec2i64 vij1 = (jp1 - ip1).cast<int64_t>();
        Vec2i64 vij2 = (jp2 - ip1).cast<int64_t>();
        int64_t tij1 = cross2(iv, vij1);
        int64_t tij2 = cross2(iv, vij2);
        return std::make_pair(
            // signum
            (tij1 > 0) ? 1 : ((tij1 < 0) ? -1 : 0),
            (tij2 > 0) ? 1 : ((tij2 < 0) ? -1 : 0));
    };

    std::pair<int, int> sign1 = segments_could_intersect(ip1, ip2, jp1, jp2);
    std::pair<int, int> sign2 = segments_could_intersect(jp1, jp2, ip1, ip2);
    int                 test1 = sign1.first * sign1.second;
    int                 test2 = sign2.first * sign2.second;
    if (test1 <= 0 && test2 <= 0) {
        // The segments possibly intersect. They may also be collinear, but not intersect.
        if (test1 != 0 || test2 != 0)
            // Certainly not collinear, then the segments intersect.
            return true;
        // If the first segment is collinear with the other, the other is collinear with the first segment.
        assert((sign1.first == 0 && sign1.second == 0) == (sign2.first == 0 && sign2.second == 0));
        if (sign1.first == 0 && sign1.second == 0) {
            // The segments are certainly collinear. Now verify whether they overlap.
            Slic3r::Point vi = ip2 - ip1;
            // Project both on the longer coordinate of vi.
            int axis = std::abs(vi.x()) > std::abs(vi.y()) ? 0 : 1;
            coord_t i = ip1(axis);
            coord_t j = ip2(axis);
            coord_t k = jp1(axis);
            coord_t l = jp2(axis);
            if (i > j)
                std::swap(i, j);
            if (k > l)
                std::swap(k, l);
            return (k >= i && k <= j) || (i >= k && i <= l);
        }
    }
    return false;
}

template<typename T> inline T foot_pt(const T &line_pt, const T &line_dir, const T &pt)
{
    T      v   = pt - line_pt;
    auto   l2  = line_dir.squaredNorm();
    auto   t   = (l2 == 0) ? 0 : v.dot(line_dir) / l2;
    return line_pt + line_dir * t;
}

inline Vec2d foot_pt(const Line &iline, const Point &ipt)
{
    return foot_pt<Vec2d>(iline.a.cast<double>(), (iline.b - iline.a).cast<double>(), ipt.cast<double>());
}

template<typename T> inline auto ray_point_distance_squared(const T &ray_pt, const T &ray_dir, const T &pt)
{
    return (foot_pt(ray_pt, ray_dir, pt) - pt).squaredNorm();
}

template<typename T> inline auto ray_point_distance(const T &ray_pt, const T &ray_dir, const T &pt)
{
    return (foot_pt(ray_pt, ray_dir, pt) - pt).norm();
}

inline double ray_point_distance_squared(const Line &iline, const Point &ipt)
{
    return (foot_pt(iline, ipt) - ipt.cast<double>()).squaredNorm();
}

inline double ray_point_distance(const Line &iline, const Point &ipt)
{
    return (foot_pt(iline, ipt) - ipt.cast<double>()).norm();
}

// Based on Liang-Barsky function by Daniel White @ http://www.skytopia.com/project/articles/compsci/clipping.html
template<typename T>
inline bool liang_barsky_line_clipping_interval(
    // Start and end points of the source line, result will be stored there as well.
    const Eigen::Matrix<T, 2, 1, Eigen::DontAlign>                  &x0,
    const Eigen::Matrix<T, 2, 1, Eigen::DontAlign>                  &v,
    // Bounding box to clip with.
    const BoundingBoxBase<Eigen::Matrix<T, 2, 1, Eigen::DontAlign>> &bbox,
    std::pair<double, double>                                       &out_interval)
{
    double t0 = 0.0;
    double t1 = 1.0;
    // Traverse through left, right, bottom, top edges.
    auto clip_side = [&t0, &t1](double p, double q) -> bool {
        if (p == 0) {
            if (q < 0)
                // Line parallel to the bounding box edge is fully outside of the bounding box.
                return false;
            // else don't clip
        } else {
            double r = q / p;
            if (p < 0) {
                if (r > t1)
                    // Fully clipped.
                    return false;
                if (r > t0)
                    // Partially clipped.
                    t0 = r;
            } else {
                assert(p > 0);
                if (r < t0)
                    // Fully clipped.
                    return false;
                if (r < t1)
                    // Partially clipped.
                    t1 = r;
            }
        }
        return true;
    };

    if (clip_side(- v.x(), - bbox.min.x() + x0.x()) &&
        clip_side(  v.x(),   bbox.max.x() - x0.x()) &&
        clip_side(- v.y(), - bbox.min.y() + x0.y()) &&
        clip_side(  v.y(),   bbox.max.y() - x0.y())) {
        out_interval.first = t0;
        out_interval.second = t1;
        return true;
    }
    return false;
}

template<typename T>
inline bool liang_barsky_line_clipping(
	// Start and end points of the source line, result will be stored there as well.
	Eigen::Matrix<T, 2, 1, Eigen::DontAlign> 						&x0,
	Eigen::Matrix<T, 2, 1, Eigen::DontAlign> 						&x1,
	// Bounding box to clip with.
	const BoundingBoxBase<Eigen::Matrix<T, 2, 1, Eigen::DontAlign>> &bbox)
{
    Eigen::Matrix<T, 2, 1, Eigen::DontAlign> v = x1 - x0;
    std::pair<double, double> interval;
    if (liang_barsky_line_clipping_interval(x0, v, bbox, interval)) {
        // Clipped successfully.
        x1  = x0 + interval.second * v;
        x0 += interval.first * v;
        return true;
    }
    return false;
}

// Based on Liang-Barsky function by Daniel White @ http://www.skytopia.com/project/articles/compsci/clipping.html
template<typename T>
bool liang_barsky_line_clipping(
	// Start and end points of the source line.
	const Eigen::Matrix<T, 2, 1, Eigen::DontAlign> 					&x0src,
	const Eigen::Matrix<T, 2, 1, Eigen::DontAlign> 					&x1src,
	// Bounding box to clip with.
	const BoundingBoxBase<Eigen::Matrix<T, 2, 1, Eigen::DontAlign>> &bbox,
	// Start and end points of the clipped line.
	Eigen::Matrix<T, 2, 1, Eigen::DontAlign> 						&x0clip,
	Eigen::Matrix<T, 2, 1, Eigen::DontAlign> 						&x1clip)
{
	x0clip = x0src;
	x1clip = x1src;
	return liang_barsky_line_clipping(x0clip, x1clip, bbox);
}

bool directions_parallel(double angle1, double angle2, double max_diff = 0);
bool directions_perpendicular(double angle1, double angle2, double max_diff = 0);
template<class T> bool contains(const std::vector<T> &vector, const Point &point);
template<typename T> T rad2deg(T angle) { return T(180.0) * angle / T(PI); }
double rad2deg_dir(double angle);
template<typename T> constexpr T deg2rad(const T angle) { return T(PI) * angle / T(180.0); }
template<typename T> T angle_to_0_2PI(T angle)
{
    static const T TWO_PI = T(2) * T(PI);
    while (angle < T(0))
    {
        angle += TWO_PI;
    }
    while (TWO_PI < angle)
    {
        angle -= TWO_PI;
    }

    return angle;
}
template<typename T> void to_range_pi_pi(T &angle){
    if (angle > T(PI) || angle <= -T(PI)) {
        int count = static_cast<int>(std::round(angle / (2 * PI)));
        angle -= static_cast<T>(count * 2 * PI);
        assert(angle <= T(PI) && angle > -T(PI));
    }
}

void simplify_polygons(const Polygons &polygons, double tolerance, Polygons* retval);

double linint(double value, double oldmin, double oldmax, double newmin, double newmax);
bool arrange(
    // input
    size_t num_parts, const Vec2d &part_size, coordf_t gap, const BoundingBoxf* bed_bounding_box, 
    // output
    Pointfs &positions);

// Sets the given transform by assembling the given transformations in the following order:
// 1) mirror
// 2) scale
// 3) rotate X
// 4) rotate Y
// 5) rotate Z
// 6) translate
void assemble_transform(Transform3d& transform, const Vec3d& translation = Vec3d::Zero(), const Vec3d& rotation = Vec3d::Zero(),
    const Vec3d& scale = Vec3d::Ones(), const Vec3d& mirror = Vec3d::Ones());

// Returns the transform obtained by assembling the given transformations in the following order:
// 1) mirror
// 2) scale
// 3) rotate X
// 4) rotate Y
// 5) rotate Z
// 6) translate
Transform3d assemble_transform(const Vec3d& translation = Vec3d::Zero(), const Vec3d& rotation = Vec3d::Zero(),
    const Vec3d& scale = Vec3d::Ones(), const Vec3d& mirror = Vec3d::Ones());

// Sets the given transform by multiplying the given transformations in the following order:
// T = translation * rotation * scale * mirror
void assemble_transform(Transform3d& transform, const Transform3d& translation = Transform3d::Identity(),
    const Transform3d& rotation = Transform3d::Identity(), const Transform3d& scale = Transform3d::Identity(),
    const Transform3d& mirror = Transform3d::Identity());

// Returns the transform obtained by multiplying the given transformations in the following order:
// T = translation * rotation * scale * mirror
Transform3d assemble_transform(const Transform3d& translation = Transform3d::Identity(), const Transform3d& rotation = Transform3d::Identity(),
    const Transform3d& scale = Transform3d::Identity(), const Transform3d& mirror = Transform3d::Identity());

// Sets the given transform by assembling the given translation
void translation_transform(Transform3d& transform, const Vec3d& translation);

// Returns the transform obtained by assembling the given translation
Transform3d translation_transform(const Vec3d& translation);

// Sets the given transform by assembling the given rotations in the following order:
// 1) rotate X
// 2) rotate Y
// 3) rotate Z
void rotation_transform(Transform3d& transform, const Vec3d& rotation);

// Returns the transform obtained by assembling the given rotations in the following order:
// 1) rotate X
// 2) rotate Y
// 3) rotate Z
Transform3d rotation_transform(const Vec3d& rotation);

// Sets the given transform by assembling the given scale factors
void scale_transform(Transform3d& transform, double scale);
void scale_transform(Transform3d& transform, const Vec3d& scale);

// Returns the transform obtained by assembling the given scale factors
Transform3d scale_transform(double scale);
Transform3d scale_transform(const Vec3d& scale);

// Returns the euler angles extracted from the given rotation matrix
// Warning -> The matrix should not contain any scale or shear !!!
Vec3d extract_euler_angles(const Eigen::Matrix<double, 3, 3, Eigen::DontAlign>& rotation_matrix);

// Returns the euler angles extracted from the given affine transform
// Warning -> The transform should not contain any shear !!!
Vec3d extract_euler_angles(const Transform3d& transform);

// get rotation from two vectors.
// Default output is axis-angle. If rotation_matrix pointer is provided, also output rotation matrix
// Euler angles can be obtained by extract_euler_angles()
void rotation_from_two_vectors(Vec3d from, Vec3d to, Vec3d &rotation_axis, double &phi, Matrix3d *rotation_matrix = nullptr);

class Transformation
{
    Transform3d m_matrix{ Transform3d::Identity() };

public:
    Transformation() = default;
    explicit Transformation(const Transform3d& transform) : m_matrix(transform) {}

    Vec3d get_offset() const { return m_matrix.translation(); }
    double get_offset(Axis axis) const { return get_offset()[axis]; }

    Transform3d get_offset_matrix() const;

    void set_offset(const Vec3d& offset) { m_matrix.translation() = offset; }
    void set_offset(Axis axis, double offset) { m_matrix.translation()[axis] = offset; }

    Vec3d get_rotation() const;
    Vec3d get_rotation_by_quaternion() const;
    double get_rotation(Axis axis) const { return get_rotation()[axis]; }

    Transform3d get_rotation_matrix() const;

    void set_rotation(const Vec3d& rotation);
    void set_rotation(Axis axis, double rotation);

    Vec3d get_scaling_factor() const;
    double get_scaling_factor(Axis axis) const { return get_scaling_factor()[axis]; }

    Transform3d get_scaling_factor_matrix() const;

    bool is_scaling_uniform() const {
        const Vec3d scale = get_scaling_factor();
        return std::abs(scale.x() - scale.y()) < 1e-8 && std::abs(scale.x() - scale.z()) < 1e-8;
    }

    void set_scaling_factor(const Vec3d& scaling_factor);
    void set_scaling_factor(Axis axis, double scaling_factor);

    Vec3d get_mirror() const;
    double get_mirror(Axis axis) const { return get_mirror()[axis]; }

    Transform3d get_mirror_matrix() const;

    bool is_left_handed() const {
        return m_matrix.linear().determinant() < 0;
    }

    void set_mirror(const Vec3d& mirror);
    void set_mirror(Axis axis, double mirror);

    bool has_skew() const;

    void reset();
    void reset_offset() { set_offset(Vec3d::Zero()); }
    void reset_rotation();
    void reset_scaling_factor();
    void reset_mirror() { set_mirror(Vec3d::Ones()); }
    void reset_skew();

    const Transform3d& get_matrix() const { return m_matrix; }
    Transform3d get_matrix_no_offset() const;
    Transform3d get_matrix_no_scaling_factor() const;

    // Orca: Implement prusa's filament shrink compensation approach
    Transform3d get_matrix_with_applied_shrinkage_compensation(const Vec3d &shrinkage_compensation) const;
    
    void set_matrix(const Transform3d& transform) { m_matrix = transform; }

    Transformation operator * (const Transformation& other) const;

    // Find volume transformation, so that the chained (instance_trafo * volume_trafo) will be as close to identity
    // as possible in least squares norm in regard to the 8 corners of bbox.
    // Bounding box is expected to be centered around zero in all axes.
    static Transformation volume_to_bed_transformation(const Transformation& instance_transformation, const BoundingBoxf3& bbox);

    // BBS: backup use this compare
    friend bool operator==(Transformation const& l, Transformation const& r) {
        return l.m_matrix.isApprox(r.m_matrix);
    }

    friend bool operator!=(Transformation const &l, Transformation const &r)
    {
        return !(l == r);
    }

private:
	friend class cereal::access;
    template<class Archive> void serialize(Archive& ar) { ar(m_matrix); }
    explicit Transformation(int) {}
    template <class Archive> static void load_and_construct(Archive& ar, cereal::construct<Transformation>& construct)
    {
        // Calling a private constructor with special "int" parameter to indicate that no construction is necessary.
        construct(1);
        ar(construct.ptr()->m_matrix);
    }
};

struct TransformationSVD
{
    Matrix3d u{ Matrix3d::Identity() };
    Matrix3d s{ Matrix3d::Identity() };
    Matrix3d v{ Matrix3d::Identity() };

    bool mirror{ false };
    bool scale{ false };
    bool anisotropic_scale{ false };
    bool rotation{ false };
    bool rotation_90_degrees{ false };
    bool skew{ false };

    explicit TransformationSVD(const Transformation& trafo) : TransformationSVD(trafo.get_matrix()) {}
    explicit TransformationSVD(const Transform3d& trafo);

    Eigen::DiagonalMatrix<double, 3, 3> mirror_matrix() const { return Eigen::DiagonalMatrix<double, 3, 3>(this->mirror ? -1. : 1., 1., 1.); }
};

// For parsing a transformation matrix from 3MF / AMF.
extern Transform3d transform3d_from_string(const std::string& transform_str);

// Rotation when going from the first coordinate system with rotation rot_xyz_from applied
// to a coordinate system with rot_xyz_to applied.
extern Eigen::Quaterniond rotation_xyz_diff(const Vec3d &rot_xyz_from, const Vec3d &rot_xyz_to);
// Rotation by Z to align rot_xyz_from to rot_xyz_to.
// This should only be called if it is known, that the two rotations only differ in rotation around the Z axis.
extern double rotation_diff_z(const Vec3d &rot_xyz_from, const Vec3d &rot_xyz_to);

// Is the angle close to a multiple of 90 degrees?
inline bool is_rotation_ninety_degrees(double a)
{
    a = fmod(std::abs(a), 0.5 * PI);
    if (a > 0.25 * PI)
        a = 0.5 * PI - a;
    return a < 0.001;
}

// Is the angle close to a multiple of 90 degrees?
inline bool is_rotation_ninety_degrees(const Vec3d &rotation)
{
    return is_rotation_ninety_degrees(rotation.x()) && is_rotation_ninety_degrees(rotation.y()) && is_rotation_ninety_degrees(rotation.z());
}

Transformation mat_around_a_point_rotate(const Transformation& innMat, const Vec3d &pt, const Vec3d &axis, float rotate_theta_radian);
Transformation generate_transform(const Vec3d &x_dir, const Vec3d &y_dir, const Vec3d &z_dir, const Vec3d &origin);

/**
 * Checks if a given point is inside a corner of a polygon.
 *
 * The corner of a polygon is defined by three points A, B, C in counterclockwise order.
 *
 * Adapted from CuraEngine LinearAlg2D::isInsideCorner by Tim Kuipers @BagelOrb
 * and @Ghostkeeper.
 *
 * @param a The first point of the corner.
 * @param b The second point of the corner (the common vertex of the two edges forming the corner).
 * @param c The third point of the corner.
 * @param query_point The point to be checked if is inside the corner.
 * @return True if the query point is inside the corner, false otherwise.
 */
bool is_point_inside_polygon_corner(const Point &a, const Point &b, const Point &c, const Point &query_point);

} } // namespace Slicer::Geometry

#endif
