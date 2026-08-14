// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_GRAD_INTRINSIC_H
#define IGL_GRAD_INTRINSIC_H
#include "igl_inline.h"

#include <Eigen/Core>
#include <Eigen/Sparse>

namespace igl {
  /// Construct an intrinsic gradient operator.
  ///
  /// @param[in] l  #F by 3 list of edge lengths
  /// @param[in] F  #F by 3 list of triangle indices into some vertex list V
  /// @param[out] G  #F*2 by #V gradient matrix: G=[Gx;Gy] where x runs along the 23 edge and
  ///    y runs in the counter-clockwise 90° rotation.
  template <typename Derivedl, typename DerivedF, typename Gtype>
  IGL_INLINE void grad_intrinsic(
    const Eigen::MatrixBase<Derivedl>&l,
    const Eigen::MatrixBase<DerivedF>&F,
    Eigen::SparseMatrix<Gtype> &G);
}
#ifndef IGL_STATIC_LIBRARY
#  include "grad_intrinsic.cpp"
#endif

#endif

