// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_COMPUTE_FRAME_FIELD_BISECTORS_H
#define IGL_COMPUTE_FRAME_FIELD_BISECTORS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Compute bisectors of a frame field defined on mesh faces
  ///
  /// @param[in] V     #V by 3 eigen Matrix of mesh vertex 3D positions
  /// @param[in] F     #F by 3 eigen Matrix of face (triangle) indices
  /// @param[in] B1    #F by 3 eigen Matrix of face (triangle) base vector 1
  /// @param[in] B2    #F by 3 eigen Matrix of face (triangle) base vector 2
  /// @param[in] PD1   #F by 3 eigen Matrix of the first per face frame field vector
  /// @param[in] PD2   #F by 3 eigen Matrix of the second per face frame field vector
  /// @param[out] BIS1  #F by 3 eigen Matrix of the first per face frame field bisector
  /// @param[out] BIS2  #F by 3 eigen Matrix of the second per face frame field bisector
  ///
  /// \bug `V` and `F` are not used. If this function is actually being used we
  /// should remove `V` and `F` here.
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE void compute_frame_field_bisectors(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedV>& B1,
    const Eigen::MatrixBase<DerivedV>& B2,
    const Eigen::MatrixBase<DerivedV>& PD1,
    const Eigen::MatrixBase<DerivedV>& PD2,
    Eigen::PlainObjectBase<DerivedV>& BIS1,
    Eigen::PlainObjectBase<DerivedV>& BIS2);
  /// \overload
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE void compute_frame_field_bisectors(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedV>& PD1,
    const Eigen::MatrixBase<DerivedV>& PD2,
    Eigen::PlainObjectBase<DerivedV>& BIS1,
    Eigen::PlainObjectBase<DerivedV>& BIS2);
}

#ifndef IGL_STATIC_LIBRARY
#  include "compute_frame_field_bisectors.cpp"
#endif

#endif
