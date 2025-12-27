#ifndef IGL_PSO_H
#define IGL_PSO_H
#include <igl/igl_inline.h>
#include <Eigen/Core>
#include <functional>

namespace igl
{
  // Solve the problem:
  //
  //   minimize f(x)
  //   subject to lb ≤ x ≤ ub 
  // 
  // by particle swarm optimization (PSO).
  //
  // Inputs:
  //   f  function that evaluates the objective for a given "particle" location
  //   LB  #X vector of lower bounds 
  //   UB  #X vector of upper bounds 
  //   max_iters  maximum number of iterations
  //   population  number of particles in swarm
  // Outputs:
  //   X  best particle seen so far
  // Returns objective corresponding to best particle seen so far
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
  // Inputs:
  //   P  whether each DOF is periodic
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
