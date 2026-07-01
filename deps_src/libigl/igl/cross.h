// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CROSS_H
#define IGL_CROSS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Cross product; out = cross(a,b)
  ///
  /// @param[in] a  left 3d vector
  /// @param[in] b  right 3d vector
  /// @param[out] out  result 3d vector
  IGL_INLINE void cross( const double *a, const double *b, double *out);
  /// Computes rowwise cross product C = cross(A,B,2);
  ///
  /// @param[in] A  #A by 3 list of row-vectors
  /// @param[in] B  #A by 3 list of row-vectors
  /// @param[out] C  #A by 3 list of row-vectors
  template <
    typename DerivedA,
    typename DerivedB,
    typename DerivedC>
  IGL_INLINE void cross(
    const Eigen::MatrixBase<DerivedA> & A,
    const Eigen::MatrixBase<DerivedB> & B,
    Eigen::PlainObjectBase<DerivedC> & C);
}

#ifndef IGL_STATIC_LIBRARY
#  include "cross.cpp"
#endif

#endif
