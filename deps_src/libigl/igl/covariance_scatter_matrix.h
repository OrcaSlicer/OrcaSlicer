// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COVARIANCE_SCATTER_MATRIX_H
#define IGL_COVARIANCE_SCATTER_MATRIX_H

#include "igl_inline.h"
#include "ARAPEnergyType.h"
#include <Eigen/Dense>
#include <Eigen/Sparse>
 
namespace igl
{
  /// Construct the covariance scatter matrix for a given arap energy
  ///
  /// @param[in] V  #V by Vdim list of initial domain positions
  /// @param[in] F  #F by 3 list of triangle indices into V
  /// @param[in] energy  ARAPEnergyType enum value defining which energy is being used.
  ///     See ARAPEnergyType.h for valid options and explanations.
  /// @param[out] CSM dim*#V/#F by dim*#V sparse matrix containing special laplacians along
  ///     the diagonal so that when multiplied by V gives covariance matrix
  ///     elements, can be used to speed up covariance matrix computation
  ///
  /// \see arap_linear_block, arap, ARAPEnergyType
  template <
    typename DerivedV, 
    typename DerivedF,
    typename CSM_type>
  IGL_INLINE void covariance_scatter_matrix(
    const Eigen::MatrixBase<DerivedV> & V, 
    const Eigen::MatrixBase<DerivedF> & F,
    const ARAPEnergyType energy,
    Eigen::SparseMatrix<CSM_type>& CSM);
}

#ifndef IGL_STATIC_LIBRARY
#include "covariance_scatter_matrix.cpp"
#endif
#endif
