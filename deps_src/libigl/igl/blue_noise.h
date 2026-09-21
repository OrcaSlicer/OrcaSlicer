// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BLUE_NOISE_H
#define IGL_BLUE_NOISE_H
#include "igl_inline.h"
#include "generate_default_urbg.h"
#include <Eigen/Core>
namespace igl
{
  /// "Fast Poisson Disk Sampling in Arbitrary Dimensions" [Bridson 2007].
  ///
  /// For very dense samplings this is faster than (up to 2x) cyCodeBase's
  /// implementation of "Sample Elimination for Generating Poisson Disk Sample
  /// Sets" [Yuksel 2015]. YMMV
  ///
  /// @param[in] V  #V by dim list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh triangle indices into rows of V
  /// @param[in] r  Poisson disk radius (evaluated according to Euclidean distance on V)
  /// @param[out] B  #P by 3 list of barycentric coordinates, ith row are coordinates of
  ///               ith sampled point in face FI(i)
  /// @param[out] FI  #P list of indices into F 
  /// @param[out] P  #P by dim list of sample positions.
  /// @param[in,out] urbg An instance of UnformRandomBitGenerator (e.g.,
  ///  `std::minstd_rand(0)`)
  /// \see random_points_on_mesh
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedB,
    typename DerivedFI,
    typename DerivedP,
    typename URBG = DEFAULT_URBG
      >
  IGL_INLINE void blue_noise(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      const typename DerivedV::Scalar r,
      Eigen::PlainObjectBase<DerivedB> & B,
      Eigen::PlainObjectBase<DerivedFI> & FI,
      Eigen::PlainObjectBase<DerivedP> & P,
      URBG && urbg = igl::generate_default_urbg());
}

#ifndef IGL_STATIC_LIBRARY
#  include "blue_noise.cpp"
#endif

#endif
