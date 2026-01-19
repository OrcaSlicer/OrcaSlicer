// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UNIQUE_SIMPLICES_H
#define IGL_UNIQUE_SIMPLICES_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  // Find *combinatorially* unique simplices in F.  **Order independent**
  //
  // Inputs:
  //   F  #F by simplex-size list of simplices
  // Outputs:
  //   FF  #FF by simplex-size list of unique simplices in F
  //   IA  #FF index vector so that FF == sort(F(IA,:),2);
  //   IC  #F index vector so that sort(F,2) == FF(IC,:);
  template <
    typename DerivedF,
    typename DerivedFF,
    typename DerivedIA,
    typename DerivedIC>
  IGL_INLINE void unique_simplices(
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedFF>& FF,
    Eigen::PlainObjectBase<DerivedIA>& IA,
    Eigen::PlainObjectBase<DerivedIC>& IC);
  template <
    typename DerivedF,
    typename DerivedFF>
  IGL_INLINE void unique_simplices(
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedFF>& FF);
}

#ifndef IGL_STATIC_LIBRARY
#  include "unique_simplices.cpp"
#endif

#endif
