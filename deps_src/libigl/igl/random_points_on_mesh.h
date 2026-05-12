// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_RANDOM_POINTS_ON_MESH_H
#define IGL_RANDOM_POINTS_ON_MESH_H

#include "igl_inline.h"
#include "generate_default_urbg.h"
#include <Eigen/Core>
#include <Eigen/Sparse>

namespace igl
{
  /// Randomly sample a mesh (V,F) n times.
  ///
  /// @param[in] n  number of samples
  /// @param[in] V  #V by dim list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh triangle indices
  /// @param[out] B  n by 3 list of barycentric coordinates, ith row are coordinates of
  ///     ith sampled point in face FI(i)
  /// @param[in] urbg An instance of UnformRandomBitGenerator (e.g.,
  ///  `std::minstd_rand(0)`)
  /// @param[out] FI  n list of indices into F 
  /// @param[in,out] urbg An instance of UnformRandomBitGenerator.
  /// @param[out] X  n by dim list of sample positions.
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedB, 
    typename DerivedFI,
    typename DerivedX,
    typename URBG = DEFAULT_URBG>
  IGL_INLINE void random_points_on_mesh(
    const int n,
    const Eigen::MatrixBase<DerivedV > & V,
    const Eigen::MatrixBase<DerivedF > & F,
    Eigen::PlainObjectBase<DerivedB > & B,
    Eigen::PlainObjectBase<DerivedFI > & FI,
    Eigen::PlainObjectBase<DerivedX> & X,
    URBG && urbg = igl::generate_default_urbg());
}

#ifndef IGL_STATIC_LIBRARY
#  include "random_points_on_mesh.cpp"
#endif

#endif
