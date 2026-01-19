// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FACES_FIRST_H
#define IGL_FACES_FIRST_H
#include "igl_inline.h"
namespace igl
{
  // FACES_FIRST Reorder vertices so that vertices in face list come before
  // vertices that don't appear in the face list. This is especially useful if
  // the face list contains only surface faces and you want surface vertices
  // listed before internal vertices
  //
  // [RV,RT,RF,IM] = faces_first(V,T,F);
  //
  // Templates:
  //   MatV  matrix for vertex positions, e.g. MatrixXd
  //   MatF  matrix for face indices, e.g. MatrixXi
  //   VecI  vector for index map, e.g. VectorXi
  // Input:
  //  V  # vertices by 3 vertex positions
  //  F  # faces by 3 list of face indices
  // Output: 
  //  RV  # vertices by 3 vertex positions, order such that if the jth vertex is
  //    some face in F, and the kth vertex is not then j comes before k
  //  RF  # faces by 3 list of face indices, reindexed to use RV
  //  IM  #V by 1 list of indices such that: RF = IM(F) and RT = IM(T)
  //    and RV(IM,:) = V
  //
  //
  // Example:
  //   // Tet mesh in (V,T,F)
  //   faces_first(V,F,IM);
  //   T = T.unaryExpr(bind1st(mem_fun( static_cast<VectorXi::Scalar&
  //     (VectorXi::*)(VectorXi::Index)>(&VectorXi::operator())),
  //     &IM)).eval();
  template <typename MatV, typename MatF, typename VecI>
  IGL_INLINE void faces_first(
    const MatV & V, 
    const MatF & F, 
    MatV & RV, 
    MatF & RF, 
    VecI & IM);
  // Virtual "in place" wrapper
  template <typename MatV, typename MatF, typename VecI>
  IGL_INLINE void faces_first(
    MatV & V, 
    MatF & F, 
    VecI & IM);
}

#ifndef IGL_STATIC_LIBRARY
#  include "faces_first.cpp"
#endif

#endif
