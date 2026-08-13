// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PROJECT_TO_LINE_SEGMENT_H
#define IGL_PROJECT_TO_LINE_SEGMENT_H
#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Project points onto vectors, that is find the parameter
  /// t for a point p such that proj_p = (y-x).*t, additionally compute the
  /// squared distance from p to the line of the vector, such that 
  /// |p - proj_p|² = sqr_d
  ///
  /// @param[in] P  #P by dim list of points to be projected
  /// @param[in] S  size dim start position of line vector
  /// @param[in] D  size dim destination position of line vector
  /// @param[out] T  #P by 1 list of parameters
  /// @param[out] sqrD  #P by 1 list of squared distances
  template <
    typename DerivedP, 
    typename DerivedS, 
    typename DerivedD, 
    typename Derivedt, 
    typename DerivedsqrD>
  IGL_INLINE void project_to_line_segment(
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedS> & S,
    const Eigen::MatrixBase<DerivedD> & D,
    Eigen::PlainObjectBase<Derivedt> & t,
    Eigen::PlainObjectBase<DerivedsqrD> & sqrD);
}

#ifndef IGL_STATIC_LIBRARY
#  include "project_to_line_segment.cpp"
#endif

#endif

