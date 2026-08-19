#ifndef IGL_PSO_H
#define IGL_PSO_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <functional>

namespace igl
{
  /// Global optimization with the particle swarm algorithm.
  ///
  /// Solve the problem:
  ///
  ///   minimize f(x)
  ///   subject to lb ≤ x ≤ ub 
  /// 
  /// by particle swarm optimization (PSO).
  ///
  /// @param[in] f  function that evaluates the objective for a given "particle" location
  /// @param[in] LB  #X vector of lower bounds 
  /// @param[in] UB  #X vector of upper bounds 
  /// @param[in] max_iters  maximum number of iterations
  /// @param[in] population  number of particles in swarm
  /// @param[out] X  best particle seen so far
  /// @return objective corresponding to best particle seen so far
  template <
    typename Scalar, 
    typename DerivedX,
    typename DerivedLB, 
    typename DerivedUB>
  IGL_INLINE Scalar pso(
    const std::function< Scalar (DerivedX &) > f,
    const Eigen::MatrixBase<DerivedLB> & LB,
    const Eigen::MatrixBase<DerivedUB> & UB,
    const int max_iters,
    const int population,
    DerivedX & X);
  /// \overload
  /// @param[out] P  whether each DOF is periodic
  ///
  /// \bug `P` appears to be unused
  template <
    typename Scalar, 
    typename DerivedX,
    typename DerivedLB, 
    typename DerivedUB,
    typename DerivedP>
  IGL_INLINE Scalar pso(
    const std::function< Scalar (DerivedX &) > f,
    const Eigen::MatrixBase<DerivedLB> & LB,
    const Eigen::MatrixBase<DerivedUB> & UB,
    const Eigen::DenseBase<DerivedP> & P,
    const int max_iters,
    const int population,
    DerivedX & X);
}

#ifndef IGL_STATIC_LIBRARY
#  include "pso.cpp"
#endif

#endif
