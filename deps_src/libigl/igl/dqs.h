// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DQS_H
#define IGL_DQS_H
#include "igl_inline.h"
#include <vector>
#include <Eigen/Core>
namespace igl
{
  /// Dual quaternion skinning
  ///
  /// @param[in] V  #V by 3 list of rest positions
  /// @param[in] W  #W by #C list of weights
  /// @param[in] vQ  #C list of rotation quaternions
  /// @param[in] vT  #C list of translation vectors
  /// @param[out] U  #V by 3 list of new positions
  template <
    typename DerivedV,
    typename DerivedW,
    typename Q,
    typename QAlloc,
    typename T,
    typename DerivedU>
  IGL_INLINE void dqs(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedW> & W,
    const std::vector<Q,QAlloc> & vQ,
    const std::vector<T> & vT,
    Eigen::PlainObjectBase<DerivedU> & U);
};

#ifndef IGL_STATIC_LIBRARY
#  include "dqs.cpp"
#endif
#endif
