// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ARAP_LINEAR_BLOCK_H
#define IGL_ARAP_LINEAR_BLOCK_H
#include "igl_inline.h"

#include <Eigen/Sparse>
#include <igl/ARAPEnergyType.h>

namespace igl
{
  // ARAP_LINEAR_BLOCK constructs a block of the matrix which constructs the
  // linear terms of a given arap energy. When treating rotations as knowns
  // (arranged in a column) then this constructs Kd of K such that the linear
  // portion of the energy is as a column:
  //   K * R = [Kx Z  ... Ky Z  ... 
  //            Z  Kx ... Z  Ky ... 
  //            ... ]
  // These blocks are also used to build the "covariance scatter matrices".
  // Here we want to build a scatter matrix that multiplies against positions
  // (treated as known) producing covariance matrices to fit each rotation.
  // Notice that in the case of the RHS of the poisson solve the rotations are
  // known and the positions unknown, and vice versa for rotation fitting.
  // These linear block just relate the rotations to the positions, linearly in
  // each.
  //
  // Templates:
  //   MatV  vertex position matrix, e.g. Eigen::MatrixXd
  //   MatF  face index matrix, e.g. Eigen::MatrixXd
  //   Scalar  e.g. double
  // Inputs:
  //   V  #V by dim list of initial domain positions
  //   F  #F by #simplex size list of triangle indices into V
  //   d  coordinate of linear constructor to build
  //   energy  ARAPEnergyType enum value defining which energy is being used.
  //     See ARAPEnergyType.h for valid options and explanations.
  // Outputs:
  //   Kd  #V by #V/#F block of the linear constructor matrix corresponding to
  //     coordinate d
  //
  template <typename MatV, typename MatF, typename Scalar>
  IGL_INLINE void arap_linear_block(
    const MatV & V,
    const MatF & F,
    const int d,
    const igl::ARAPEnergyType energy,
    Eigen::SparseMatrix<Scalar> & Kd);
  // Helper functions for each energy type
  template <typename MatV, typename MatF, typename Scalar>
  IGL_INLINE void arap_linear_block_spokes(
    const MatV & V,
    const MatF & F,
    const int d,
    Eigen::SparseMatrix<Scalar> & Kd);
  template <typename MatV, typename MatF, typename Scalar>
  IGL_INLINE void arap_linear_block_spokes_and_rims(
    const MatV & V,
    const MatF & F,
    const int d,
    Eigen::SparseMatrix<Scalar> & Kd);
  template <typename MatV, typename MatF, typename Scalar>
  IGL_INLINE void arap_linear_block_elements(
    const MatV & V,
    const MatF & F,
    const int d,
    Eigen::SparseMatrix<Scalar> & Kd);
}

#ifndef IGL_STATIC_LIBRARY
#  include "arap_linear_block.cpp"
#endif

#endif
