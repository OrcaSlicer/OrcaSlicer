// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Olga Diamanti, Alec Jacobson
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_NCHOOSEK
#define IGL_NCHOOSEK
#include "igl_inline.h"
#include "deprecated.h"
#include <vector>

#include <Eigen/Core>

namespace igl 
{
  // NCHOOSEK  Like matlab's nchoosek.
  //
  // Inputs:
  //   n  total number elements
  //   k  size of sub-set to consider
  // Returns number of k-size combinations out of the set [1,...,n]
  IGL_INLINE double nchoosek(const int n, const int k);
  // 
  // Inputs:
  //   V  n-long vector of elements
  //   k  size of sub-set to consider
  // Outputs:
  //   U  nchoosek by k long matrix where each row is a unique k-size
  //     combination
  template < typename DerivedV, typename DerivedU>
  IGL_INLINE void nchoosek(
    const Eigen::MatrixBase<DerivedV> & V,
    const int k,
    Eigen::PlainObjectBase<DerivedU> & U);
}


#ifndef IGL_STATIC_LIBRARY
#include "nchoosek.cpp"
#endif


#endif /* defined(IGL_NCHOOSEK) */
