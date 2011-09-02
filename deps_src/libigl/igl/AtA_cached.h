// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2017 Daniele Panozzo <daniele.panozzo@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ATA_CACHED_H
#define IGL_ATA_CACHED_H
#include "igl_inline.h"
#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>
namespace igl
{  
  struct AtA_cached_data
  {
    // Weights
    Eigen::VectorXd W;

    // Flatten composition rules
    std::vector<int> I_row;
    std::vector<int> I_col;
    std::vector<int> I_w;

    // For each entry of AtA, points to the beginning
    // of the composition rules
    std::vector<int> I_outer;
  };

  // Computes At * W * A, where A is sparse and W is diagonal. Divides the 
  // construction in two phases, one
  // for fixing the sparsity pattern, and one to populate it with values. Compared to
  // evaluating it directly, this version is slower for the first time (since it requires a
  // precomputation), but faster to the subsequent evaluations.
  //
  // Input:
  //   A m x n sparse matrix
  //   data stores the precomputed sparsity pattern, data.W contains the optional diagonal weights (stored as a dense vector). If W is not provided, it is replaced by the identity.
  // Outputs:
  //   AtA  m by m matrix computed as AtA * W * A
  //
  // Example:
  // AtA_data = igl::AtA_cached_data();
  // AtA_data.W = W;
  // if (s.AtA.rows() == 0)
  //   igl::AtA_cached_precompute(s.A,s.AtA_data,s.AtA);
  // else
  //   igl::AtA_cached(s.A,s.AtA_data,s.AtA);
  template <typename Scalar>
  IGL_INLINE void AtA_cached_precompute(
    const Eigen::SparseMatrix<Scalar>& A,
    AtA_cached_data& data,
    Eigen::SparseMatrix<Scalar>& AtA
    );

  template <typename Scalar>
  IGL_INLINE void AtA_cached(
    const Eigen::SparseMatrix<Scalar>& A,
    const AtA_cached_data& data,
    Eigen::SparseMatrix<Scalar>& AtA
    );
  
}

#ifndef IGL_STATIC_LIBRARY
#  include "AtA_cached.cpp"
#endif

#endif
