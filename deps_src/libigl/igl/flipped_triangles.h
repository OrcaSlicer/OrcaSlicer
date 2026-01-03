// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Michael Rabinovich <michaelrabinovich27@gmail.com@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FLIPPED_TRIANGLES_H
#define IGL_FLIPPED_TRIANGLES_H
#include "igl_inline.h"

#include <Eigen/Dense>
namespace igl
{
  /// Finds the ids of the flipped triangles of the mesh V,F in the UV mapping uv
  ///
  /// @param[in] V  #V by 2 list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh faces (must be triangles)
  /// @param[out] X  #flipped list of containing the indices into F of the flipped triangles
  template <typename DerivedV, typename DerivedF, typename DerivedX>
  IGL_INLINE void flipped_triangles(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedX> & X);
  /// \overload
  template <typename Scalar, typename Index>
  IGL_INLINE Eigen::VectorXi flipped_triangles(
    const Eigen::MatrixBase<Scalar> & V,
    const Eigen::MatrixBase<Index> & F);

}

#ifndef IGL_STATIC_LIBRARY
#  include "flipped_triangles.cpp"
#endif

#endif
