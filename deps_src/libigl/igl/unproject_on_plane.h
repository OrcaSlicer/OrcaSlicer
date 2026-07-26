#ifndef IGL_UNPROJECT_ON_PLANE_H
#define IGL_UNPROJECT_ON_PLANE_H

#include <Eigen/Core>

namespace igl
{
  /// Given a screen space point (u,v) and the current projection matrix (e.g.
  /// gl_proj * gl_modelview) and viewport, _unproject_ the point into the scene
  /// so that it lies on given plane.
  ///
  /// @param[in] UV  2-long uv-coordinates of screen space point
  /// @param[in] M  4 by 4 projection matrix
  /// @param[in] VP  4-long viewport: (corner_u, corner_v, width, height)
  /// @param[in] P  4-long plane equation coefficients: P*(X 1) = 0
  /// @param[out] Z  3-long world coordinate
  template <
    typename DerivedUV,
    typename DerivedM,
    typename DerivedVP,
    typename DerivedP,
    typename DerivedZ>
  void unproject_on_plane(
    const Eigen::MatrixBase<DerivedUV> & UV,
    const Eigen::MatrixBase<DerivedM> & M,
    const Eigen::MatrixBase<DerivedVP> & VP,
    const Eigen::MatrixBase<DerivedP> & P,
    Eigen::PlainObjectBase<DerivedZ> & Z);
}

#ifndef IGL_STATIC_LIBRARY
#  include "unproject_on_plane.cpp"
#endif

#endif
