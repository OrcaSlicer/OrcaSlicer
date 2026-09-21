#ifndef IGL_TURNING_NUMBER_H
#define IGL_TURNING_NUMBER_H
#include <Eigen/Core>
#include "igl_inline.h"

namespace igl
{
  /// Compute the turning number of a closed curve in the plane.
  ///
  /// @param[in] V  #V by 2 list of vertex positions
  /// @return tn  turning number
  ///
  template <typename DerivedV>
  IGL_INLINE typename DerivedV::Scalar turning_number(
    const Eigen::MatrixBase<DerivedV> & V);
}

#ifndef IGL_STATIC_LIBRARY
#  include "turning_number.cpp"
#endif

#endif
