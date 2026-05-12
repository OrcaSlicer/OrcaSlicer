#ifndef IGL_MIN_H
#define IGL_MIN_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  /// Compute the minimum along dimension dim of a matrix X
  ///
  /// @param[in] X  m by n matrix
  /// @param[in] dim  dimension along which to take min
  /// @param[out] Y
  ///   n-long vector (if dim == 1) 
  ///   Y  m-long vector (if dim == 2)
  /// @param[out] I  vector the same size as Y containing the indices along dim of minimum
  ///     entries
  template <typename AType, typename DerivedB, typename DerivedI>
  IGL_INLINE void min(
    const Eigen::SparseMatrix<AType> & A,
    const int dim,
    Eigen::PlainObjectBase<DerivedB> & B,
    Eigen::PlainObjectBase<DerivedI> & I);
  /// \overload
  template <typename DerivedX, typename DerivedY, typename DerivedI>
  IGL_INLINE void min(
    const Eigen::DenseBase<DerivedX> & X,
    const int dim,
    Eigen::PlainObjectBase<DerivedY> & Y,
    Eigen::PlainObjectBase<DerivedI> & I);
}
#ifndef IGL_STATIC_LIBRARY
#  include "min.cpp"
#endif
#endif

