// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_RANDPERM_H
#define IGL_RANDPERM_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Like matlab's randperm(n) but minus 1
  //
  // Inputs:
  //   n  number of elements
  // Outputs:
  //   I  n list of rand permutation of 0:n-1
  template <typename DerivedI>
  IGL_INLINE void randperm(
    const int n,
    Eigen::PlainObjectBase<DerivedI> & I);
}
#ifndef IGL_STATIC_LIBRARY
#  include "randperm.cpp"
#endif
#endif
