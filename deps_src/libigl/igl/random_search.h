#ifndef IGL_RANDOM_SEARCH_H
#define IGL_RANDOM_SEARCH_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <functional>
namespace igl
{
  /// Global optimization via random search.
  ///
  /// Solve the problem:
  ///
  ///   minimize f(x)
  ///   subject to lb ≤ x ≤ ub 
  /// 
  /// by uniform random search.
  ///
  /// @param[in] f  function to minimize
  /// @param[in] LB  #X vector of finite lower bounds
  /// @param[in] UB  #X vector of finite upper bounds
  /// @param[in] iters  number of iterations
  /// @param[out] X  #X optimal parameter vector
  /// @return f(X)
  ///
  template <
    typename Scalar, 
    typename DerivedX, 
    typename DerivedLB, 
    typename DerivedUB>
  IGL_INLINE Scalar random_search(
    const std::function< Scalar (DerivedX &) > f,
    const Eigen::MatrixBase<DerivedLB> & LB,
    const Eigen::MatrixBase<DerivedUB> & UB,
    const int iters,
    DerivedX & X);
}

#ifndef IGL_STATIC_LIBRARY
#  include "random_search.cpp"
#endif

#endif

