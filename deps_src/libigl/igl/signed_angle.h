#ifndef IGL_SIGNED_ANGLE_H
#define IGL_SIGNED_ANGLE_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  // Compute the signed angle subtended by the oriented 3d triangle (A,B,C) at some point P
  // 
  // Inputs:
  //   A  2D position of corner 
  //   B  2D position of corner 
  //   P  2D position of query point
  //   returns signed angle
  template <
    typename DerivedA,
    typename DerivedB,
    typename DerivedP>
  IGL_INLINE typename DerivedA::Scalar signed_angle(
    const Eigen::MatrixBase<DerivedA> & A,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<DerivedP> & P);
}
#ifndef IGL_STATIC_LIBRARY
#  include "signed_angle.cpp"
#endif
#endif

