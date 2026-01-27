#ifndef slic3r_Geometry_ArcWelder_hpp_
#define slic3r_Geometry_ArcWelder_hpp_

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <Eigen/Geometry>
#include <type_traits>
#include <cassert>

#include "libslic3r/libslic3r.h"

namespace Slic3r { namespace Geometry { namespace ArcWelder {

// Calculate center point (center of a circle) of an arc given two points and a radius.
// positive radius: take shorter arc
// negative radius: take longer arc
// radius must NOT be zero!
template<typename Derived, typename Derived2, typename Float>
inline Eigen::Matrix<Float, 2, 1, Eigen::DontAlign> arc_center(
    const Eigen::MatrixBase<Derived>   &start_pos,
    const Eigen::MatrixBase<Derived2>  &end_pos, 
    const Float                         radius,
    const bool                          is_ccw)
{
    static_assert(Derived::IsVectorAtCompileTime && int(Derived::SizeAtCompileTime) == 2, "arc_center(): first parameter is not a 2D vector");
    static_assert(Derived2::IsVectorAtCompileTime && int(Derived2::SizeAtCompileTime) == 2, "arc_center(): second parameter is not a 2D vector");
    static_assert(std::is_same<typename Derived::Scalar, typename Derived2::Scalar>::value, "arc_center(): Both vectors must be of the same type.");
    static_assert(std::is_same<typename Derived::Scalar, Float>::value, "arc_center(): Radius must be of the same type as the vectors.");
    assert(radius != 0);
    using Vector = Eigen::Matrix<Float, 2, 1, Eigen::DontAlign>;
    auto  v  = end_pos - start_pos;
    Float q2 = v.squaredNorm();
    assert(q2 > 0);
    Float t2 = sqr(radius) / q2 - Float(.25f);
    // If the start_pos and end_pos are nearly antipodal, t2 may become slightly negative.
    // In that case return a centroid of start_point & end_point.
    Float t = t2 > 0 ? sqrt(t2) : Float(0);
    auto mid = Float(0.5) * (start_pos + end_pos);
    Vector vp{ -v.y() * t, v.x() * t };
    return (radius > Float(0)) == is_ccw ? (mid + vp).eval() : (mid - vp).eval();
}


// Return number of linear segments necessary to interpolate arc of a given positive radius and positive angle to satisfy
// maximum deviation of an interpolating polyline from an analytic arc.
template<typename FloatType>
size_t arc_discretization_steps(const FloatType radius, const FloatType angle, const FloatType deviation)
{
    assert(radius > 0);
    assert(angle > 0);
    assert(angle <= FloatType(2. * M_PI));
    assert(deviation > 0);

    FloatType d = radius - deviation;
    return d < EPSILON ?
        // Radius smaller than deviation.
        (   // Acute angle: a single segment interpolates the arc with sufficient accuracy.
            angle < M_PI || 
            // Obtuse angle: Test whether the furthest point (center) of an arc is closer than deviation to the center of a line segment.
            radius * (FloatType(1.) + cos(M_PI - FloatType(.5) * angle)) < deviation ?
            // Single segment is sufficient
            1 :
            // Two segments are necessary, the middle point is at the center of the arc.
            2) :
        size_t(ceil(angle / (2. * acos(d / radius))));
}

} } } // namespace Slic3r::Geometry::ArcWelder

#endif // slic3r_Geometry_ArcWelder_hpp_
