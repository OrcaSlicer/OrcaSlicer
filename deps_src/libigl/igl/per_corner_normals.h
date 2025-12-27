// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PER_CORNER_NORMALS_H
#define IGL_PER_CORNER_NORMALS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  // Compute vertex normals via vertex position list, face list
  // Inputs:
  //   V  #V by 3 eigen Matrix of mesh vertex 3D positions
  //   F  #F by 3 eigen Matrix of face (triangle) indices
  //   corner_threshold  threshold in degrees on sharp angles
  // Output:
  //   CN  #F*3 by 3 eigen Matrix of mesh vertex 3D normals, where the normal
  //     for corner F(i,j) is at CN(i*3+j,:) 
  template <typename DerivedV, typename DerivedF, typename DerivedCN>
  IGL_INLINE void per_corner_normals(
    const Eigen::PlainObjectBase<DerivedV>& V,
    const Eigen::PlainObjectBase<DerivedF>& F,
    const double corner_threshold,
    Eigen::PlainObjectBase<DerivedCN> & CN);
  // Other Inputs:
  //   FN  #F by 3 eigen Matrix of face normals
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedFN, 
    typename DerivedCN>
  IGL_INLINE void per_corner_normals(
    const Eigen::PlainObjectBase<DerivedV>& V,
    const Eigen::PlainObjectBase<DerivedF>& F,
    const Eigen::PlainObjectBase<DerivedFN>& FN,
    const double corner_threshold,
    Eigen::PlainObjectBase<DerivedCN> & CN);
  // Other Inputs:
  //   VF  map from vertices to list of incident faces
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedFN, 
    typename IndexType,
    typename DerivedCN>
  IGL_INLINE void per_corner_normals(
    const Eigen::PlainObjectBase<DerivedV>& V,
    const Eigen::PlainObjectBase<DerivedF>& F,
    const Eigen::PlainObjectBase<DerivedFN>& FN,
    const std::vector<std::vector<IndexType> >& VF,
    const double corner_threshold,
    Eigen::PlainObjectBase<DerivedCN> & CN);
}

#ifndef IGL_STATIC_LIBRARY
#  include "per_corner_normals.cpp"
#endif

#endif
