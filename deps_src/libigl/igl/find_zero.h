#ifndef IGL_FIND_ZERO_H
#define IGL_FIND_ZERO_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  /// Find the first zero (whether implicit or explicitly stored) in the
  /// rows/columns of a matrix.
  ///
  /// @param[in] A  m by n sparse matrix
  /// @param[in] dim  dimension along which to check for any (1 or 2)
  /// @param[out] I  n-long vector (if dim == 1)  {m means no zeros found}
  ///   or m-long vector (if dim == 2)  {n means no zeros found}
  ///
  template <typename AType, typename DerivedI>
  IGL_INLINE void find_zero(
    const Eigen::SparseMatrix<AType> & A,
    const int dim,
    Eigen::PlainObjectBase<DerivedI> & I);
}
#ifndef IGL_STATIC_LIBRARY
#  include "find_zero.cpp"
#endif
#endif

