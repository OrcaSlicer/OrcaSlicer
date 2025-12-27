// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ACTIVE_SET_H
#define IGL_ACTIVE_SET_H

#include "igl_inline.h"
#include "SolverStatus.h"
#include <Eigen/Core>
#include <Eigen/Sparse>

namespace igl
{
  struct active_set_params;
  // Known Bugs: rows of [Aeq;Aieq] **must** be linearly independent. Should be
  // using QR decomposition otherwise:
  //   http://www.okstate.edu/sas/v8/sashtml/ormp/chap5/sect32.htm
  //
  // ACTIVE_SET Minimize quadratic energy 
  //
  // 0.5*Z'*A*Z + Z'*B + C with constraints
  //
  // that Z(known) = Y, optionally also subject to the constraints Aeq*Z = Beq,
  // and further optionally subject to the linear inequality constraints that
  // Aieq*Z <= Bieq and constant inequality constraints lx <= x <= ux
  //
  // Inputs:
  //   A  n by n matrix of quadratic coefficients
  //   B  n by 1 column of linear coefficients
  //   known  list of indices to known rows in Z
  //   Y  list of fixed values corresponding to known rows in Z
  //   Aeq  meq by n list of linear equality constraint coefficients
  //   Beq  meq by 1 list of linear equality constraint constant values
  //   Aieq  mieq by n list of linear inequality constraint coefficients
  //   Bieq  mieq by 1 list of linear inequality constraint constant values
  //   lx  n by 1 list of lower bounds [] implies -Inf
  //   ux  n by 1 list of upper bounds [] implies Inf
  //   params  struct of additional parameters (see below)
  //   Z  if not empty, is taken to be an n by 1 list of initial guess values
  //     (see output)
  // Outputs:
  //   Z  n by 1 list of solution values
  // Returns true on success, false on error
  //
  // Benchmark: For a harmonic solve on a mesh with 325K facets, matlab 2.2
  // secs, igl/min_quad_with_fixed.h 7.1 secs
  //
  template <
    typename AT, 
    typename DerivedB,
    typename Derivedknown, 
    typename DerivedY,
    typename AeqT,
    typename DerivedBeq,
    typename AieqT,
    typename DerivedBieq,
    typename Derivedlx,
    typename Derivedux,
    typename DerivedZ
    >
  IGL_INLINE igl::SolverStatus active_set(
    const Eigen::SparseMatrix<AT>& A,
    const Eigen::PlainObjectBase<DerivedB> & B,
    const Eigen::PlainObjectBase<Derivedknown> & known,
    const Eigen::PlainObjectBase<DerivedY> & Y,
    const Eigen::SparseMatrix<AeqT>& Aeq,
    const Eigen::PlainObjectBase<DerivedBeq> & Beq,
    const Eigen::SparseMatrix<AieqT>& Aieq,
    const Eigen::PlainObjectBase<DerivedBieq> & Bieq,
    const Eigen::PlainObjectBase<Derivedlx> & lx,
    const Eigen::PlainObjectBase<Derivedux> & ux,
    const igl::active_set_params & params,
    Eigen::PlainObjectBase<DerivedZ> & Z
    );
};

#include "EPS.h"
struct igl::active_set_params
{
  // Input parameters for active_set:
  //   Auu_pd  whether Auu is positive definite {false}
  //   max_iter  Maximum number of iterations (0 = Infinity, {100})
  //   inactive_threshold  Threshold on Lagrange multiplier values to determine
  //     whether to keep constraints active {EPS}
  //   constraint_threshold  Threshold on whether constraints are violated (0
  //     is perfect) {EPS}
  //   solution_diff_threshold  Threshold on the squared norm of the difference
  //     between two consecutive solutions {EPS}
  bool Auu_pd;
  int max_iter;
  double inactive_threshold;
  double constraint_threshold;
  double solution_diff_threshold;
  active_set_params():
    Auu_pd(false),
    max_iter(100),
    inactive_threshold(igl::DOUBLE_EPS),
    constraint_threshold(igl::DOUBLE_EPS),
    solution_diff_threshold(igl::DOUBLE_EPS)
    {};
};

#ifndef IGL_STATIC_LIBRARY
#  include "active_set.cpp"
#endif

#endif
