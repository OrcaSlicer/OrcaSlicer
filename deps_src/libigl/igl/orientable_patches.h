// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ORIENTABLE_PATCHES_H
#define IGL_ORIENTABLE_PATCHES_H
#include <igl/igl_inline.h>
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  //  Compute connected components of facets connected by manifold edges.
  // 
  //  Known bugs: This will detect a moebius strip as a single patch (manifold,
  //  non-orientable) and also non-manfiold, yet orientable patches. 
  // 
  //  Q: Does this find exactly (manifold || orientable) patches?
  // 
  //  Inputs:
  //    F  #F by simplex-size list of facets
  //  Outputs:
  //    C  #F list of component ids
  //    A  #F by #F adjacency matrix
  // 
  template <typename DerivedF, typename DerivedC, typename AScalar>
  IGL_INLINE void orientable_patches(
    const Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedC> & C,
    Eigen::SparseMatrix<AScalar> & A);
  template <typename DerivedF, typename DerivedC>
  IGL_INLINE void orientable_patches(
    const Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedC> & C);
};
#ifndef IGL_STATIC_LIBRARY
#  include "orientable_patches.cpp"
#endif
#endif
