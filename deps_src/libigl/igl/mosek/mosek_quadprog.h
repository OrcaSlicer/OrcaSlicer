// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MOSEK_MOSEK_QUADPROG_H
#define IGL_MOSEK_MOSEK_QUADPROG_H
#include "../igl_inline.h"
#include <vector>
#include <map>
#include <mosek.h>


#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl
{
  namespace mosek
  {
    /// Structure for holding MOSEK data for solving a quadratic program
    struct MosekData
    {
      /// Integer parameters
      std::map<MSKiparame,int> intparam;
      /// Double parameters
      std::map<MSKdparame,double> douparam;
      /// Default values
      IGL_INLINE MosekData();
    };
    // Solve a convex quadratic optimization problem with linear and constant
    // bounds. Given in the form:
    //
    //      Minimize: ½ * xT * Q⁰ * x + cT * x + cf
    //
    //      Subject to: lc ≤ Ax ≤ uc
    //                  lx ≤ x ≤ ux
    //
    // where we are trying to find the optimal vector of values x. 
    //
    // \note Q⁰ must be symmetric
    //
    // \note Because of how MOSEK accepts different parts of the system, Q
    // should be stored in IJV (aka Coordinate) format and should only include
    // entries in the lower triangle. A should be stored in Column compressed
    // (aka Harwell Boeing) format. As described:
    // http://netlib.org/linalg/html_templates/node92.html
    // or
    // http://en.wikipedia.org/wiki/Sparse_matrix
    //   #Compressed_sparse_column_.28CSC_or_CCS.29
    // 
    //
    // @tparam Index  type for index variables
    // @tparam Scalar  type for floating point variables (gets cast to double?)
    // @param[in] n  number of variables, i.e. size of x
    // @param[in] Qi  vector of qnnz row indices of non-zeros in LOWER TRIANGLE ONLY of
    //       Q⁰
    // @param[in] Qj  vector of qnnz column indices of non-zeros in LOWER TRIANGLE ONLY
    //       of Q⁰
    // @param[in] Qv  vector of qnnz values of non-zeros in LOWER TRIANGLE ONLY of Q⁰, 
    //       such that:
    //
    //           Q⁰(Qi[k],Qj[k]) = Qv[k] for k ∈ [0,Qnnz-1], where Qnnz is the
    // 
    //       number of non-zeros in Q⁰
    // @param[in] c   (optional) vector of n values of c, transpose of coefficient row
    //       vector of linear terms, EMPTY means c == 0
    // @param[in] cf  (ignored) value of constant term in objective, 0 means cf == 0, so
    //       optional only in the sense that it is mandatory
    // @param[in] m  number of constraints, therefore also number of rows in linear
    //      constraint coefficient matrix A, and in linear constraint bound
    //      vectors lc and uc
    // @param[in] Av  vector of non-zero values of A, in column compressed order
    // @param[in] Ari  vector of row indices corresponding to non-zero values of A,
    // @param[in] Acp  vector of indices into Ari and Av of the first entry for each
    //        column of A, size(Acp) = (# columns of A) + 1 = n + 1
    // @param[in] lc  vector of m linear constraint lower bounds
    // @param[in] uc  vector of m linear constraint upper bounds
    // @param[in] lx  vector of n constant lower bounds
    // @param[in] ux  vector of n constant upper bounds
    // @param[out] x  vector of size n to hold output of optimization
    // @return true only if optimization was successful with no errors
    // 
    // \note All indices are 0-based
    template <typename Index, typename Scalar>
    IGL_INLINE bool mosek_quadprog(
      const Index n,
      /* mosek won't allow this to be const*/ std::vector<Index> & Qi,
      /* mosek won't allow this to be const*/ std::vector<Index> & Qj,
      /* mosek won't allow this to be const*/ std::vector<Scalar> & Qv,
      const std::vector<Scalar> & c,
      const Scalar cf,
      const Index m,
      /* mosek won't allow this to be const*/ std::vector<Scalar> & Av,
      /* mosek won't allow this to be const*/ std::vector<Index> & Ari,
      const std::vector<Index> & Acp,
      const std::vector<Scalar> & lc,
      const std::vector<Scalar> & uc,
      const std::vector<Scalar> & lx,
      const std::vector<Scalar> & ux,
      MosekData & mosek_data,
      std::vector<Scalar> & x);
    /// \overload 
    ///
    /// @param[in] Q  n by n square quadratic coefficients matrix **only lower triangle
    ///      is used**.
    /// @param[in] c  n-long vector of linear coefficients
    /// @param[in] cf  constant coefficient
    /// @param[in] A  m by n square linear coefficienst matrix of inequality constraints
    /// @param[in] lc  m-long vector of lower bounds for linear inequality constraints
    /// @param[in] uc  m-long vector of upper bounds for linear inequality constraints
    /// @param[in] lx  n-long vector of lower bounds
    /// @param[in] ux  n-long vector of upper bounds
    /// @param[in] mosek_data  parameters struct
    /// @param[out] x  n-long solution vector
    /// @return true only if optimization finishes without error
    ///
    IGL_INLINE bool mosek_quadprog(
      const Eigen::SparseMatrix<double> & Q,
      const Eigen::VectorXd & c,
      const double cf,
      const Eigen::SparseMatrix<double> & A,
      const Eigen::VectorXd & lc,
      const Eigen::VectorXd & uc,
      const Eigen::VectorXd & lx,
      const Eigen::VectorXd & ux,
      MosekData & mosek_data,
      Eigen::VectorXd & x);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "mosek_quadprog.cpp"
#endif

#endif
