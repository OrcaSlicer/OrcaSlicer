// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COMBINE_H
#define IGL_COMBINE_H

#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  // Concatenate k meshes into a single >=k connected component mesh with a
  // single vertex list and face list. Similar to Maya's Combine operation. 
  //
  // Inputs:
  //   VV  k-long list of lists of mesh vertex positions
  //   FF  k-long list of lists of mesh face indices so that FF[i] indexes
  //     VV[i]
  // Outputs:
  //   V   VV[0].rows()+...+VV[k-1].rows() by VV[0].cols() list of mesh
  //     vertex positions
  //   F   FF[0].rows()+...+FF[k-1].rows() by FF[0].cols() list of mesh faces
  //     indices into V
  //   Vsizes  k list so that Vsizes(i) is the #vertices in the ith input
  //   Fsizes  k list so that Fsizes(i) is the #faces in the ith input
  // Example:
  //   // Suppose you have mesh A (VA,FA) and mesh B (VB,FB)
  //   igl::combine<Eigen::MatrixXd,Eigen::MatrixXi>({VA,VB},{FA,FB},V,F);
  //
  //
  template <
    typename DerivedVV, 
    typename DerivedFF, 
    typename DerivedV, 
    typename DerivedF,
    typename DerivedVsizes,
    typename DerivedFsizes>
  IGL_INLINE void combine(
    const std::vector<DerivedVV> & VV,
    const std::vector<DerivedFF> & FF,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedVsizes> & Vsizes,
    Eigen::PlainObjectBase<DerivedFsizes> & Fsizes);
  template <
    typename DerivedVV, 
    typename DerivedFF, 
    typename DerivedV, 
    typename DerivedF>
  IGL_INLINE void combine(
    const std::vector<DerivedVV> & VV,
    const std::vector<DerivedFF> & FF,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F);
}

#ifndef IGL_STATIC_LIBRARY
#  include "combine.cpp"
#endif
#endif
