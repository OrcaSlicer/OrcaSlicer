// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FACE_AREAS_H
#define IGL_FACE_AREAS_H
#include "igl_inline.h"

#include <Eigen/Dense>

namespace igl
{
  // Constructs a list of face areas of faces opposite each index in a tet list
  //
  // Inputs:
  //   V  #V by 3 list of mesh vertex positions
  //   T  #T by 3 list of tet mesh indices into V
  // Outputs:
  //   A   #T by 4 list of face areas corresponding to faces opposite vertices
  //     0,1,2,3
  //
  template <typename DerivedV, typename DerivedT, typename DerivedA>
  IGL_INLINE void face_areas(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedT>& T,
    Eigen::PlainObjectBase<DerivedA>& A);
  // Compute tet-mesh face areas from edge lengths.
  //
  // Inputs:
  //   L  #T by 6 list of tet-mesh edge lengths corresponding to edges
  //     [3 0],[3 1],[3 2],[1 2],[2 0],[0 1]
  // Outputs:
  //   A   #T by 4 list of face areas corresponding to faces opposite vertices 
  //     0,1,2,3: i.e. made of edges [123],[024],[015],[345]
  //    
  //
  template <typename DerivedL, typename DerivedA>
  IGL_INLINE void face_areas(
    const Eigen::MatrixBase<DerivedL>& L,
    Eigen::PlainObjectBase<DerivedA>& A);
  // doublearea_nan_replacement  See doublearea.h
  template <typename DerivedL, typename DerivedA>
  IGL_INLINE void face_areas(
    const Eigen::MatrixBase<DerivedL>& L,
    const typename DerivedL::Scalar doublearea_nan_replacement,
    Eigen::PlainObjectBase<DerivedA>& A);
}

#ifndef IGL_STATIC_LIBRARY
#  include "face_areas.cpp"
#endif

#endif


