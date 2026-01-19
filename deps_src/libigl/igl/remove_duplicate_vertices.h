// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_REMOVE_DUPLICATE_VERTICES_H
#define IGL_REMOVE_DUPLICATE_VERTICES_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  // REMOVE_DUPLICATE_VERTICES Remove duplicate vertices upto a uniqueness
  // tolerance (epsilon)
  //
  // Inputs:
  //   V  #V by dim list of vertex positions
  //   epsilon  uniqueness tolerance (significant digit), can probably think of
  //     this as a tolerance on L1 distance
  // Outputs:
  //   SV  #SV by dim new list of vertex positions
  //   SVI #V by 1 list of indices so SV = V(SVI,:) 
  //   SVJ #SV by 1 list of indices so V = SV(SVJ,:)
  //
  // Example:
  //   % Mesh in (V,F)
  //   [SV,SVI,SVJ] = remove_duplicate_vertices(V,1e-7);
  //   % remap faces
  //   SF = SVJ(F);
  //
  template <
    typename DerivedV, 
    typename DerivedSV, 
    typename DerivedSVI, 
    typename DerivedSVJ>
  IGL_INLINE void remove_duplicate_vertices(
    const Eigen::MatrixBase<DerivedV>& V,
    const double epsilon,
    Eigen::PlainObjectBase<DerivedSV>& SV,
    Eigen::PlainObjectBase<DerivedSVI>& SVI,
    Eigen::PlainObjectBase<DerivedSVJ>& SVJ);
  // Wrapper that also remaps given faces (F) --> (SF) so that SF index SV
  template <
    typename DerivedV, 
    typename DerivedF,
    typename DerivedSV, 
    typename DerivedSVI, 
    typename DerivedSVJ,
    typename DerivedSF>
  IGL_INLINE void remove_duplicate_vertices(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const double epsilon,
    Eigen::PlainObjectBase<DerivedSV>& SV,
    Eigen::PlainObjectBase<DerivedSVI>& SVI,
    Eigen::PlainObjectBase<DerivedSVJ>& SVJ,
    Eigen::PlainObjectBase<DerivedSF>& SF);
}

#ifndef IGL_STATIC_LIBRARY
#  include "remove_duplicate_vertices.cpp"
#endif

#endif
