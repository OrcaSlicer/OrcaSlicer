#ifndef IGL_SOLID_ANGLE_H
#define IGL_SOLID_ANGLE_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  /// Compute the signed solid angle subtended by the oriented 3d triangle (A,B,C) at some point P
  /// 
  /// @param[in] A  3D position of corner 
  /// @param[in] B  3D position of corner 
  /// @param[in] C  3D position of corner 
  /// @param[in] P  3D position of query point
  /// @return signed solid angle
  template <
    typename DerivedA,
    typename DerivedB,
    typename DerivedC,
    typename DerivedP>
  IGL_INLINE typename DerivedA::Scalar solid_angle(
    const Eigen::MatrixBase<DerivedA> & A,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<DerivedC> & C,
    const Eigen::MatrixBase<DerivedP> & P);
}
#ifndef IGL_STATIC_LIBRARY
#  include "solid_angle.cpp"
#endif
#endif
