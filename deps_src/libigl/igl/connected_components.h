// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CONNECTED_COMPONENTS_H
#define IGL_CONNECTED_COMPONENTS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  /// Determine the connected components of a graph described by the input
  /// adjacency matrix (similar to MATLAB's graphconncomp or gptoolbox's
  /// conncomp, but A is transposed for unsymmetric graphs).
  ///
  /// @param[in]  A  #A by #A adjacency matrix (treated as describing an directed graph)
  /// @param[out] C  #A list of component indices into [0,#K-1]
  /// @param[out] K  #K list of sizes of each component
  /// @return number of connected components
  template < typename Atype, typename DerivedC, typename DerivedK>
  IGL_INLINE int connected_components(
    const Eigen::SparseMatrix<Atype> & A,
    Eigen::PlainObjectBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedK> & K);
}

#ifndef IGL_STATIC_LIBRARY
#  include "connected_components.cpp"
#endif

#endif
