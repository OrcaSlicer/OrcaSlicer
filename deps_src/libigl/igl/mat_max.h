// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MAT_MAX_H
#define IGL_MAT_MAX_H
#include "igl_inline.h"
#include <Eigen/Dense>

namespace igl
{
  // Ideally this becomes a super overloaded function supporting everything
  // that matlab's max supports

  // Max function for matrices to act like matlab's max function. Specifically
  // like [Y,I] = max(X,[],dim);
  //
  // Templates:
  //   T  should be a eigen matrix primitive type like int or double
  // Inputs:
  //   X  m by n matrix
  //   dim  dimension along which to take max
  // Outputs:
  //   Y  n-long vector (if dim == 1) 
  //   or
  //   Y  m-long vector (if dim == 2)
  //   I  vector the same size as Y containing the indices along dim of maximum
  //     entries
  template <typename DerivedX, typename DerivedY, typename DerivedI>
  IGL_INLINE void mat_max(
    const Eigen::DenseBase<DerivedX> & X,
    const int dim,
    Eigen::PlainObjectBase<DerivedY> & Y,
    Eigen::PlainObjectBase<DerivedI> & I);
}

#ifndef IGL_STATIC_LIBRARY
#  include "mat_max.cpp"
#endif

#endif
