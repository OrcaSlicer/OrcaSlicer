// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COLUMNIZE_H
#define IGL_COLUMNIZE_H
#include "igl_inline.h"

#include <Eigen/Core>
namespace igl
{
  // "Columnize" a stack of block matrices. If A = [A1,A2,A3,...,Ak] with each A*
  // an m by n block then this produces the column vector whose entries are 
  // B(j*m*k+i*k+b) = A(i,b*n+j);
  // or if A = [A1;A2;...;Ak] then
  // B(j*m*k+i*k+b) = A(i+b*m,j);
  //
  // Templates:
  //   T  should be a eigen matrix primitive type like int or double
  // Inputs:
  //   A  m*k by n (dim: 1) or m by n*k (dim: 2) eigen Matrix of type T values
  //   k  number of blocks
  //   dim  dimension in which blocks are stacked
  // Output
  //   B  m*n*k eigen vector of type T values,
  //
  // See also: transpose_blocks
  template <typename DerivedA, typename DerivedB>
  IGL_INLINE void columnize(
    const Eigen::PlainObjectBase<DerivedA> & A,
    const int k,
    const int dim,
    Eigen::PlainObjectBase<DerivedB> & B);
}
#ifndef IGL_STATIC_LIBRARY
#  include "columnize.cpp"
#endif
#endif
