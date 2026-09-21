// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_ROTATE_VECTORS_H
#define IGL_ROTATE_VECTORS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Rotate the vectors V by A radians on the tangent plane spanned by B1 and
  /// B2
  ///
  /// @param[in] V     #V by 3 eigen Matrix of vectors
  /// @param[in] A     #V eigen vector of rotation angles or a single angle to be applied
  ///     to all vectors
  /// @param[in] B1    #V by 3 eigen Matrix of base vector 1
  /// @param[in] B2    #V by 3 eigen Matrix of base vector 2
  /// @return the rotated vectors
  ///
  IGL_INLINE Eigen::MatrixXd rotate_vectors(
    const Eigen::MatrixXd& V,
    const Eigen::VectorXd& A,
    const Eigen::MatrixXd& B1,
    const Eigen::MatrixXd& B2);
}

#ifndef IGL_STATIC_LIBRARY
#  include "rotate_vectors.cpp"
#endif

#endif
