#ifndef IGL_UNPROJECT_ON_LINE_H
#define IGL_UNPROJECT_ON_LINE_H

#include <Eigen/Dense>

namespace igl
{
  /// Given a screen space point (u,v) and the current projection matrix (e.g.
  /// gl_proj * gl_modelview) and viewport, _unproject_ the point into the scene
  /// so that it lies on given line (origin and dir) and projects as closely as
  /// possible to the given screen space point.
  ///
  /// @param[in] UV  2-long uv-coordinates of screen space point
  /// @param[in] M  4 by 4 projection matrix
  /// @param[in] VP  4-long viewport: (corner_u, corner_v, width, height)
  /// @param[in] origin  point on line
  /// @param[in] dir  vector parallel to line
  /// @param[out] t  line parameter so that closest poin on line to viewer ray through UV
  ///     lies at origin+t*dir
  template <
    typename DerivedUV,
    typename DerivedM,
    typename DerivedVP,
    typename Derivedorigin,
    typename Deriveddir>
  void unproject_on_line(
    const Eigen::MatrixBase<DerivedUV> & UV,
    const Eigen::MatrixBase<DerivedM> & M,
    const Eigen::MatrixBase<DerivedVP> & VP,
    const Eigen::MatrixBase<Derivedorigin> & origin,
    const Eigen::MatrixBase<Deriveddir> & dir,
    typename DerivedUV::Scalar & t);
  /// \overload
  /// @param[out] Z  3d position of closest point on line to viewing ray through UV
  template <
    typename DerivedUV,
    typename DerivedM,
    typename DerivedVP,
    typename Derivedorigin,
    typename Deriveddir,
    typename DerivedZ>
  void unproject_on_line(
    const Eigen::MatrixBase<DerivedUV> & UV,
    const Eigen::MatrixBase<DerivedM> & M,
    const Eigen::MatrixBase<DerivedVP> & VP,
    const Eigen::MatrixBase<Derivedorigin> & origin,
    const Eigen::MatrixBase<Deriveddir> & dir,
    Eigen::PlainObjectBase<DerivedZ> & Z);
}

#ifndef IGL_STATIC_LIBRARY
#  include "unproject_on_line.cpp"
#endif

#endif
