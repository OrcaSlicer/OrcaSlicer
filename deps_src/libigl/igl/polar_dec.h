// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_POLAR_DEC
#define IGL_POLAR_DEC
#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  // Computes the polar decomposition (R,T) of a matrix A
  // Inputs:
  //   A  3 by 3 matrix to be decomposed
  // Outputs:
  //   R  3 by 3 orthonormal matrix part of decomposition
  //   T  3 by 3 stretch matrix part of decomposition
  //   U  3 by 3 left-singular vectors
  //   S  3 by 1 singular values
  //   V  3 by 3 right-singular vectors
  //
  //
  template <
    typename DerivedA,
    typename DerivedR,
    typename DerivedT,
    typename DerivedU,
    typename DerivedS,
    typename DerivedV>
  IGL_INLINE void polar_dec(
    const Eigen::PlainObjectBase<DerivedA> & A,
    Eigen::PlainObjectBase<DerivedR> & R,
    Eigen::PlainObjectBase<DerivedT> & T,
    Eigen::PlainObjectBase<DerivedU> & U,
    Eigen::PlainObjectBase<DerivedS> & S,
    Eigen::PlainObjectBase<DerivedV> & V);
  template <
    typename DerivedA,
    typename DerivedR,
    typename DerivedT>
  IGL_INLINE void polar_dec(
    const Eigen::PlainObjectBase<DerivedA> & A,
    Eigen::PlainObjectBase<DerivedR> & R,
    Eigen::PlainObjectBase<DerivedT> & T);
}
#ifndef IGL_STATIC_LIBRARY
#  include "polar_dec.cpp"
#endif
#endif

