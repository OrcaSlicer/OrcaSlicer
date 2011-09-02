#ifndef IGL_MAX_H
#define IGL_MAX_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  // Inputs:
  //   X  m by n matrix
  //   dim  dimension along which to take max
  // Outputs:
  //   Y  n-long vector (if dim == 1) 
  //   or
  //   Y  m-long vector (if dim == 2)
  //   I  vector the same size as Y containing the indices along dim of maximum
  //     entries
  template <typename AType, typename DerivedB, typename DerivedI>
  IGL_INLINE void max(
    const Eigen::SparseMatrix<AType> & A,
    const int dim,
    Eigen::PlainObjectBase<DerivedB> & B,
    Eigen::PlainObjectBase<DerivedI> & I);
}
#ifndef IGL_STATIC_LIBRARY
#  include "max.cpp"
#endif
#endif
