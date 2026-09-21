#ifndef IGL_RAY_TRIANGLE_INTERSECT_H
#define IGL_RAY_TRIANGLE_INTERSECT_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Determine whether (and if so where) a ray intersects a triangle at a
  /// single point.
  ///
  /// @param[in] O  3d origin of ray
  /// @param[in] D  3d direction of ray
  /// @param[in] V0 3d position of first triangle vertex
  /// @param[in] V1 3d position of second triangle vertex
  /// @param[in] V2 3d position of third triangle vertex
  /// @param[in] epsilon  epsilon for determining whether ray is parallel to
  ///  triangle
  /// @param[out] t  distance along ray to intersection (if any)
  /// @param[out] u  barycentric coordinate of V1 triangle vertex
  /// @param[out] v  barycentric coordinate of V2 triangle vertex
  /// @param[out] parallel whether ray was considered parallel to triangle (and
  ///   if so then will return false)
  /// @returns true if ray intersects triangle
  ///
  ///
  template <
    typename DerivedO,
    typename DerivedD,
    typename DerivedV0,
    typename DerivedV1,
    typename DerivedV2>
  IGL_INLINE bool ray_triangle_intersect(
     const Eigen::MatrixBase<DerivedO> & O,
     const Eigen::MatrixBase<DerivedD> & D,
     const Eigen::MatrixBase<DerivedV0> & V0,
     const Eigen::MatrixBase<DerivedV1> & V1,
     const Eigen::MatrixBase<DerivedV2> & V2,
     const typename DerivedO::Scalar epsilon,
     typename DerivedO::Scalar & t,
     typename DerivedO::Scalar & u,
     typename DerivedO::Scalar & v,
     bool & parallel);
}

#ifndef IGL_STATIC_LIBRARY
#  include "ray_triangle_intersect.cpp"
#endif

#endif
