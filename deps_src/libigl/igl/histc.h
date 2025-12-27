// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_HISTC_H
#define IGL_HISTC_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  // Like matlab's histc. Count occurrences of values in X between consecutive
  // entries in E
  //
  // Inputs:
  //   X  m-long Vector of values
  //   E  n-long Monotonically increasing vector of edges
  // Outputs:
  //   N  n-long vector where N(k) reveals how many values in X fall between
  //     E(k) <= X < E(k+1)
  //   B  m-long vector of bin ids so that B(j) = k if E(k) <= X(j) < E(k+1).
  //     B(j) = -1 if X(j) is outside of E.
  //
  // O(n+m*log(n))
  template <typename DerivedX, typename DerivedE, typename DerivedN, typename DerivedB>
  IGL_INLINE void histc(
    const Eigen::MatrixBase<DerivedX > & X,
    const Eigen::MatrixBase<DerivedE > & E,
    Eigen::PlainObjectBase<DerivedN > & N,
    Eigen::PlainObjectBase<DerivedB > & B);
  // Truly O(m*log(n))
  template <typename DerivedX, typename DerivedE, typename DerivedB>
  IGL_INLINE void histc(
    const Eigen::MatrixBase<DerivedX > & X,
    const Eigen::MatrixBase<DerivedE > & E,
    Eigen::PlainObjectBase<DerivedB > & B);
  // Scalar search wrapper
  template <typename DerivedE>
  IGL_INLINE void histc(
    const typename DerivedE::Scalar & x,
    const Eigen::MatrixBase<DerivedE > & E,
    typename DerivedE::Index & b);
}

#ifndef IGL_STATIC_LIBRARY
#  include "histc.cpp"
#endif

#endif



