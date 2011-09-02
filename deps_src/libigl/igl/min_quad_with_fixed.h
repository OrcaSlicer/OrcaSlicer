// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MIN_QUAD_WITH_FIXED_H
#define IGL_MIN_QUAD_WITH_FIXED_H
#include "igl_inline.h"

#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/Sparse>
// Bug in unsupported/Eigen/SparseExtra needs iostream first
#include <iostream>
#include <unsupported/Eigen/SparseExtra>

namespace igl
{
  template <typename T>
  struct min_quad_with_fixed_data;
  // Known Bugs: rows of Aeq **should probably** be linearly independent.
  // During precomputation, the rows of a Aeq are checked via QR. But in case
  // they're not then resulting probably will no longer be sparse: it will be
  // slow.
  //
  // MIN_QUAD_WITH_FIXED Minimize a quadratic energy of the form
  //
  // trace( 0.5*Z'*A*Z + Z'*B + constant )
  //
  // subject to
  //
  //   Z(known,:) = Y, and
  //   Aeq*Z = Beq
  //
  // Templates:
  //   T  should be a eigen matrix primitive type like int or double
  // Inputs:
  //   A  n by n matrix of quadratic coefficients
  //   known list of indices to known rows in Z
  //   Y  list of fixed values corresponding to known rows in Z
  //   Aeq  m by n list of linear equality constraint coefficients
  //   pd flag specifying whether A(unknown,unknown) is positive definite
  // Outputs:
  //   data  factorization struct with all necessary information to solve
  //     using min_quad_with_fixed_solve
  // Returns true on success, false on error
  //
  // Benchmark: For a harmonic solve on a mesh with 325K facets, matlab 2.2
  // secs, igl/min_quad_with_fixed.h 7.1 secs
  //
  template <typename T, typename Derivedknown>
  IGL_INLINE bool min_quad_with_fixed_precompute(
    const Eigen::SparseMatrix<T>& A,
    const Eigen::MatrixBase<Derivedknown> & known,
    const Eigen::SparseMatrix<T>& Aeq,
    const bool pd,
    min_quad_with_fixed_data<T> & data
    );
  // Solves a system previously factored using min_quad_with_fixed_precompute
  //
  // Template:
  //   T  type of sparse matrix (e.g. double)
  //   DerivedY  type of Y (e.g. derived from VectorXd or MatrixXd)
  //   DerivedZ  type of Z (e.g. derived from VectorXd or MatrixXd)
  // Inputs:
  //   data  factorization struct with all necessary precomputation to solve
  //   B  n by k column of linear coefficients
  //   Y  b by k list of constant fixed values
  //   Beq  m by k list of linear equality constraint constant values
  // Outputs:
  //   Z  n by k solution
  //   sol  #unknowns+#lagrange by k solution to linear system
  // Returns true on success, false on error
  template <
    typename T,
    typename DerivedB,
    typename DerivedY,
    typename DerivedBeq,
    typename DerivedZ,
    typename Derivedsol>
  IGL_INLINE bool min_quad_with_fixed_solve(
    const min_quad_with_fixed_data<T> & data,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<DerivedY> & Y,
    const Eigen::MatrixBase<DerivedBeq> & Beq,
    Eigen::PlainObjectBase<DerivedZ> & Z,
    Eigen::PlainObjectBase<Derivedsol> & sol);
  // Wrapper without sol
  template <
    typename T,
    typename DerivedB,
    typename DerivedY,
    typename DerivedBeq,
    typename DerivedZ>
  IGL_INLINE bool min_quad_with_fixed_solve(
    const min_quad_with_fixed_data<T> & data,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<DerivedY> & Y,
    const Eigen::MatrixBase<DerivedBeq> & Beq,
    Eigen::PlainObjectBase<DerivedZ> & Z);
  template <
    typename T,
    typename Derivedknown,
    typename DerivedB,
    typename DerivedY,
    typename DerivedBeq,
    typename DerivedZ>
  IGL_INLINE bool min_quad_with_fixed(
    const Eigen::SparseMatrix<T>& A,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<Derivedknown> & known,
    const Eigen::MatrixBase<DerivedY> & Y,
    const Eigen::SparseMatrix<T>& Aeq,
    const Eigen::MatrixBase<DerivedBeq> & Beq,
    const bool pd,
    Eigen::PlainObjectBase<DerivedZ> & Z);
}

template <typename T>
struct igl::min_quad_with_fixed_data
{
  // Size of original system: number of unknowns + number of knowns
  int n;
  // Whether A(unknown,unknown) is positive definite
  bool Auu_pd;
  // Whether A(unknown,unknown) is symmetric
  bool Auu_sym;
  // Indices of known variables
  Eigen::VectorXi known;
  // Indices of unknown variables
  Eigen::VectorXi unknown;
  // Indices of lagrange variables
  Eigen::VectorXi lagrange;
  // Indices of unknown variable followed by Indices of lagrange variables
  Eigen::VectorXi unknown_lagrange;
  // Matrix multiplied against Y when constructing right hand side
  Eigen::SparseMatrix<T> preY;
  enum SolverType
  {
    LLT = 0,
    LDLT = 1,
    LU = 2,
    QR_LLT = 3,
    NUM_SOLVER_TYPES = 4
  } solver_type;
  // Solvers
  Eigen::SimplicialLLT <Eigen::SparseMatrix<T > > llt;
  Eigen::SimplicialLDLT<Eigen::SparseMatrix<T > > ldlt;
  Eigen::SparseLU<Eigen::SparseMatrix<T, Eigen::ColMajor>, Eigen::COLAMDOrdering<int> >   lu;
  // QR factorization
  // Are rows of Aeq linearly independent?
  bool Aeq_li;
  // Columns of Aeq corresponding to unknowns
  int neq;
  Eigen::SparseQR<Eigen::SparseMatrix<T>, Eigen::COLAMDOrdering<int> >  AeqTQR;
  Eigen::SparseMatrix<T> Aeqk;
  Eigen::SparseMatrix<T> Aequ;
  Eigen::SparseMatrix<T> Auu;
  Eigen::SparseMatrix<T> AeqTQ1;
  Eigen::SparseMatrix<T> AeqTQ1T;
  Eigen::SparseMatrix<T> AeqTQ2;
  Eigen::SparseMatrix<T> AeqTQ2T;
  Eigen::SparseMatrix<T> AeqTR1;
  Eigen::SparseMatrix<T> AeqTR1T;
  Eigen::SparseMatrix<T> AeqTE;
  Eigen::SparseMatrix<T> AeqTET;
  // Debug
  Eigen::SparseMatrix<T> NA;
  Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> NB;
};

#ifndef IGL_STATIC_LIBRARY
#  include "min_quad_with_fixed.cpp"
#endif

#endif
