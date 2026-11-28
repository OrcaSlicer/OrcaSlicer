#ifndef IGL_GRID_SEARCH_H
#define IGL_GRID_SEARCH_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <functional>
namespace igl
{
  /// Global optimization via grid search. Solve the problem:
  ///
  ///   minimize f(x)
  ///   subject to lb ≤ x ≤ ub 
  /// 
  /// by exhaustive grid search.
  ///
  /// @param[in] f  function to minimize
  /// @param[in] LB  #X vector of finite lower bounds
  /// @param[in] UB  #X vector of finite upper bounds
  /// @param[in] I  #X vector of number of steps for each variable
  /// @param[out] X  #X optimal parameter vector
  /// @return f(X)
  ///
  template <
    typename Scalar, 
    typename DerivedX, 
    typename DerivedLB, 
    typename DerivedUB, 
    typename DerivedI>
  IGL_INLINE Scalar grid_search(
    const std::function< Scalar (DerivedX &) > f,
    const Eigen::MatrixBase<DerivedLB> & LB,
    const Eigen::MatrixBase<DerivedUB> & UB,
    const Eigen::MatrixBase<DerivedI> & I,
    DerivedX & X);
}

#ifndef IGL_STATIC_LIBRARY
#  include "grid_search.cpp"
#endif

#endif
