// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COMPONENTS_H
#define IGL_COMPONENTS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  // Compute connected components of a graph represented by an adjacency
  // matrix. This version is faster than the previous version using boost.
  //
  // Inputs:
  //   A  n by n adjacency matrix
  // Outputs:
  //   C  n list of component ids (starting with 0)
  //   counts  #components list of counts for each component
  //
  template <typename AScalar, typename DerivedC, typename Derivedcounts>
  IGL_INLINE void components(
    const Eigen::SparseMatrix<AScalar> & A,
    Eigen::PlainObjectBase<DerivedC> & C,
    Eigen::PlainObjectBase<Derivedcounts> & counts);
  template <typename AScalar, typename DerivedC>
  IGL_INLINE void components(
    const Eigen::SparseMatrix<AScalar> & A,
    Eigen::PlainObjectBase<DerivedC> & C);
  // Ditto but for mesh faces as input. This computes connected components of
  // **vertices** where **edges** establish connectivity.
  //
  // Inputs:
  //   F  n by 3 list of triangle indices
  // Outputs:
  //   C  max(F) list of component ids
  template <typename DerivedF, typename DerivedC>
  IGL_INLINE void components(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedC> & C);

}

#ifndef IGL_STATIC_LIBRARY
#  include "components.cpp"
#endif

#endif

