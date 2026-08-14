// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
//  and Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_HESSIAN_H
#define IGL_HESSIAN_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>


namespace igl
{
  /// Constructs the finite element Hessian matrix
  /// as described in https://arxiv.org/abs/1707.04348,
  /// Natural Boundary Conditions for Smoothing in Geometry Processing
  /// (Oded Stein, Eitan Grinspun, Max Wardetzky, Alec Jacobson)
  /// The interior vertices are NOT set to zero yet.
  ///
  /// @param[in] V  #V by dim list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh faces (must be triangles)
  /// @param[out] H  dim²⋅#V by #V Hessian matrix, each column i
  ///     corresponding to V(i,:)
  ///
  ///
  /// \see curved_hessian_energy, hessian_energy
  ///
  template <typename DerivedV, typename DerivedF, typename Scalar>
  IGL_INLINE void hessian(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::SparseMatrix<Scalar>& H);
  
}

#ifndef IGL_STATIC_LIBRARY
#  include "hessian.cpp"
#endif

#endif
