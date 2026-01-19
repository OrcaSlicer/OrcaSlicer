// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DOT_ROW_H
#define IGL_DOT_ROW_H

#include "igl/igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  // Compute the dot product between each row of A and B
  // Templates:
  //   DerivedV derived from vertex positions matrix type: i.e. MatrixXd
  // Inputs:
  //   A  eigen matrix r by c
  //   B  eigen matrix r by c
  // Returns:
  //   d a column vector with r entries that contains the dot product of each corresponding row of A and B
  //
  template <typename DerivedV>
  IGL_INLINE DerivedV dot_row(
    const Eigen::PlainObjectBase<DerivedV>& A,
    const Eigen::PlainObjectBase<DerivedV>& B);

}

#ifndef IGL_STATIC_LIBRARY
#  include "dot_row.cpp"
#endif

#endif
