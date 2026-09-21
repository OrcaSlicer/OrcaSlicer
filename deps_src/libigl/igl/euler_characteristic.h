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
  /// Computes the Euler characteristic of a given mesh (V,F)
  ///
  /// χ = |V| - |E| + |F|
  ///
  /// For example,
  ///   - a single triangle has 3 - 3 + 1 = 1
  ///   - a tetrahedron has 4 - 6 + 4 = 2
  ///
  /// @param[in] F #F by dim list of mesh faces (must be triangles)
  /// @return An int containing the Euler characteristic
  ///
  /// \note There is [some confusion](https://github.com/libigl/libigl/issues/2249#issue-1863608449) over the standard definition of Euler
  /// characteristic. libigl's definition agrees with
  /// [wikipedia](https://en.wikipedia.org/wiki/Euler_characteristic) and [David
  /// Eppstein's
  /// proofs](https://www.ics.uci.edu/~eppstein/junkyard/euler/all.html).
  template <typename DerivedF>
  IGL_INLINE int euler_characteristic(
    const Eigen::MatrixBase<DerivedF> & F);
}

#ifndef IGL_STATIC_LIBRARY
#  include "euler_characteristic.cpp"
#endif

#endif
