#ifndef IGL_PROJECTION_CONSTRAINT_H
#define IGL_PROJECTION_CONSTRAINT_H

#include <Eigen/Core>

namespace igl
{
  /// Construct two constraint equations for projecting a point to the screen. Of the form:
  ///
  ///     A z = B
  ///
  /// with A 2x3 and B 2x1, where z is the 3d position of point in the scene,
  /// given the current projection matrix (e.g. gl_proj * gl_modelview), viewport
  /// (corner u/v and width/height) and screen space point x,y. Satisfying this
  /// equation means that z projects to screen space point (x,y).
  /// 
  /// @param[in] UV  2-long uv-coordinates of screen space point
  /// @param[in] M  4 by 4 projection matrix
  /// @param[in] VP  4-long viewport: (corner_u, corner_v, width, height)
  /// @param[out] A  2 by 3 system matrix
  /// @param[out] B  2 by 1 right-hand side
  template <
    typename DerivedUV,
    typename DerivedM,
    typename DerivedVP,
    typename DerivedA,
    typename DerivedB>
  void projection_constraint(
    const Eigen::MatrixBase<DerivedUV> & UV,
    const Eigen::MatrixBase<DerivedM> & M,
    const Eigen::MatrixBase<DerivedVP> & VP,
    Eigen::PlainObjectBase<DerivedA> & A,
    Eigen::PlainObjectBase<DerivedB> & B);
}

#ifndef IGL_STATIC_LIBRARY
#  include "projection_constraint.cpp"
#endif

#endif

