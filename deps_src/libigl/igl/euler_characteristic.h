// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Michael Rabinovich <michaelrabinovich27@gmail.com>
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EULER_CHARACTERISTIC_H
#define IGL_EULER_CHARACTERISTIC_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <vector>
namespace igl
{
  // Computes the Euler characteristic of a given mesh (V,F)
  //
  // Inputs:
  //   F #F by dim list of mesh faces (must be triangles)
  // Returns An int containing the Euler characteristic
  template <typename DerivedF>
  IGL_INLINE int euler_characteristic(
    const Eigen::MatrixBase<DerivedF> & F);

  // Computes the Euler characteristic of a given mesh (V,F)
  // Templates:
  //   Scalar  should be a floating point number type
  //   Index   should be an integer type
  // Inputs:
  //   V       #V by dim list of mesh vertex positions
  //   F       #F by dim list of mesh faces (must be triangles)
  // Returns An int containing the Euler characteristic
  template <typename Scalar, typename Index>
  IGL_INLINE int euler_characteristic(
    const Eigen::PlainObjectBase<Scalar> & V,
    const Eigen::PlainObjectBase<Index> & F);

}

#ifndef IGL_STATIC_LIBRARY
#  include "euler_characteristic.cpp"
#endif

#endif
