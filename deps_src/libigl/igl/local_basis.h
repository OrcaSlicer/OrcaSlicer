// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_LOCALBASIS_H
#define IGL_LOCALBASIS_H

#include "igl_inline.h"
#include <Eigen/Core>
#include <string>
#include <vector>

namespace igl 
{
  /// Compute a local orthogonal reference system for each triangle in the given mesh
  ///
  /// @tparam DerivedV derived from vertex positions matrix type: i.e. MatrixXd
  /// @tparam DerivedF derived from face indices matrix type: i.e. MatrixXi
  /// @param[in] V  eigen matrix #V by 3
  /// @param[in] F  #F by 3 list of mesh faces (must be triangles)
  /// @param[out] B1 eigen matrix #F by 3, each vector is tangent to the triangle
  /// @param[out] B2 eigen matrix #F by 3, each vector is tangent to the triangle and perpendicular to B1
  /// @param[out] B3 eigen matrix #F by 3, normal of the triangle
  ///
  template <
    typename DerivedV, 
    typename DerivedF,
    typename DerivedB1,
    typename DerivedB2,
    typename DerivedB3>
  IGL_INLINE void local_basis(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedB1>& B1,
    Eigen::PlainObjectBase<DerivedB2>& B2,
    Eigen::PlainObjectBase<DerivedB3>& B3
    );

}

#ifndef IGL_STATIC_LIBRARY
#  include "local_basis.cpp"
#endif

#endif
