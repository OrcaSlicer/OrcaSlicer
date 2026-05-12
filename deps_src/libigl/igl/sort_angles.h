// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef SORT_ANGLES_H
#define SORT_ANGLES_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl {
  /// Sort angles in ascending order in a numerically robust way.
  ///
  /// Instead of computing angles using atan2(y, x), sort directly on (y, x).
  ///
  /// @param[in] M: m by n matrix of scalars. (n >= 2).  Assuming the first
  ///   column of M contains values for y, and the second column is x.  Using
  ///   the rest of the columns as tie-breaker.
  /// @param[in] R: an array of m indices.  M.row(R[i]) contains the i-th
  ///   smallest angle.
  template<typename DerivedM, typename DerivedR>
  IGL_INLINE void sort_angles(
    const Eigen::MatrixBase<DerivedM>& M,
    Eigen::PlainObjectBase<DerivedR>& R);
}

#ifndef IGL_STATIC_LIBRARY
#include "sort_angles.cpp"
#endif

#endif
