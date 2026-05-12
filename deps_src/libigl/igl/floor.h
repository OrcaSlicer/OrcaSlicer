// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FLOOR_H
#define IGL_FLOOR_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  /// Floor a given matrix to nearest integers 
  ///
  /// @param[in] X  m by n matrix of scalars
  /// @param[out] Y  m by n matrix of floored integers
  template < typename DerivedX, typename DerivedY>
  IGL_INLINE void floor(
    const Eigen::DenseBase<DerivedX>& X,
    Eigen::PlainObjectBase<DerivedY>& Y);
}

#ifndef IGL_STATIC_LIBRARY
#  include "floor.cpp"
#endif

#endif
