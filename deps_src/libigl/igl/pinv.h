#ifndef IGL_PINV_H
#define IGL_PINV_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Compute the Moore-Penrose pseudoinverse
  ///
  /// @param[in] A  m by n matrix 
  /// @param[in] tol  tolerance (if negative then default is used)
  /// @param[out] X  n by m matrix so that A*X*A = A and X*A*X = X and A*X =
  ///   (A*X)' and (X*A) = (X*A)'
  ///
  /// \deprecated Use `Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd>`
  /// `.solve()` or `.pseudoinverse()` instead.
  template <typename DerivedA, typename DerivedX>
  void pinv(
    const Eigen::MatrixBase<DerivedA> & A,
    typename DerivedA::Scalar tol,
    Eigen::PlainObjectBase<DerivedX> & X);
  /// \overload
  template <typename DerivedA, typename DerivedX>
  void pinv(
    const Eigen::MatrixBase<DerivedA> & A,
    Eigen::PlainObjectBase<DerivedX> & X);
}
#ifndef IGL_STATIC_LIBRARY
#  include "pinv.cpp"
#endif
#endif
