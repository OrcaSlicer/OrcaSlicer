// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ARAP_RHS_H
#define IGL_ARAP_RHS_H
#include "igl_inline.h"
#include "ARAPEnergyType.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl
{
  /// Right-hand side constructor of global poisson solve for various Arap
  /// energies
  ///
  /// @param[in] V  #V by Vdim list of initial domain positions
  /// @param[in] F  #F by 3 list of triangle indices into V
  /// @param[in] dim  dimension being used at solve time. For deformation usually dim =
  ///     V.cols(), for surface parameterization V.cols() = 3 and dim = 2
  /// @param[in] energy  igl::ARAPEnergyType enum value defining which energy is being
  ///     used. See igl::ARAPEnergyType.h for valid options and explanations.
  /// @param[out] K  #V*dim by #(F|V)*dim*dim matrix such that:
  ///     b = K * reshape(permute(R,[3 1 2]),size(V|F,1)*size(V,2)*size(V,2),1);
  ///
  /// \see arap_linear_block
  template<typename DerivedV, typename DerivedF, typename DerivedK>
  IGL_INLINE void arap_rhs(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const int dim,
    const igl::ARAPEnergyType energy,
    Eigen::SparseCompressedBase<DerivedK>& K);
}
#ifndef IGL_STATIC_LIBRARY
#include "arap_rhs.cpp"
#endif
#endif
