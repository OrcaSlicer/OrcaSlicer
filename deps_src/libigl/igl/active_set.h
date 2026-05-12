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
  ///
  /// Minimize convex quadratic energy subject to linear inequality constraints
  ///
  ///     min ½ Zᵀ A Z + Zᵀ B + constant
  ///      Z
  ///     subject to
  ///            Aeq Z = Beq
  ///            Aieq Z <= Bieq
  ///            lx <= Z <= ux
  ///            Z(known) = Y
  ///
  /// that Z(known) = Y, optionally also subject to the constraints Aeq*Z = Beq,
  /// and further optionally subject to the linear inequality constraints that
  /// Aieq*Z <= Bieq and constant inequality constraints lx <= x <= ux
  ///
  /// @param[in] A  n by n matrix of quadratic coefficients
  /// @param[in] B  n by 1 column of linear coefficients
  /// @param[in] known  list of indices to known rows in Z
  /// @param[in] Y  list of fixed values corresponding to known rows in Z
  /// @param[in] Aeq  meq by n list of linear equality constraint coefficients
  /// @param[in] Beq  meq by 1 list of linear equality constraint constant values
  /// @param[in] Aieq  mieq by n list of linear inequality constraint coefficients
  /// @param[in] Bieq  mieq by 1 list of linear inequality constraint constant values
  /// @param[in] lx  n by 1 list of lower bounds [] implies -Inf
  /// @param[in] ux  n by 1 list of upper bounds [] implies Inf
  /// @param[in] params  struct of additional parameters (see below)
  /// @param[in,out] Z  if not empty, is taken to be an n by 1 list of initial guess values. Set to solution on output.
  /// @return true on success, false on error
  ///
  /// \note Benchmark: For a harmonic solve on a mesh with 325K facets, matlab 2.2
  /// secs, igl/min_quad_with_fixed.h 7.1 secs
  ///
  /// \pre rows of [Aeq;Aieq] **must** be linearly independent. Should be
  /// using QR decomposition otherwise:
  /// https://v8doc.sas.com/sashtml/ormp/chap5/sect32.htm
  ///
  /// \warning This solver is fairly experimental. It works reasonably well for
  /// bbw problems but doesn't generalize well to other problems.  NASOQ and
  /// OSQP are better general purpose solvers.
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
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<Derivedknown> & known,
    const Eigen::MatrixBase<DerivedY> & Y,
    const Eigen::SparseMatrix<AeqT>& Aeq,
    const Eigen::MatrixBase<DerivedBeq> & Beq,
    const Eigen::SparseMatrix<AieqT>& Aieq,
    const Eigen::MatrixBase<DerivedBieq> & Bieq,
    const Eigen::MatrixBase<Derivedlx> & lx,
    const Eigen::MatrixBase<Derivedux> & ux,
    const igl::active_set_params & params,
    Eigen::PlainObjectBase<DerivedZ> & Z
    );
};

#include "EPS.h"
/// Input parameters controling active_set
///
/// \fileinfo
struct igl::active_set_params
{
///  Auu_pd  whether Auu is positive definite {false}
  bool Auu_pd;
///  max_iter  Maximum number of iterations (0 = Infinity, {100})
  int max_iter;
///  inactive_threshold  Threshold on Lagrange multiplier values to determine
///   whether to keep constraints active {EPS}
  double inactive_threshold;
///  constraint_threshold  Threshold on whether constraints are violated (0
///   is perfect) {EPS}
  double constraint_threshold;
///  solution_diff_threshold  Threshold on the squared norm of the difference
///    between two consecutive solutions {EPS}
  double solution_diff_threshold;
  /// @private
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
