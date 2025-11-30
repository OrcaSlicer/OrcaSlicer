// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COLLAPSE_SMALL_TRIANGLES_H
#define IGL_COLLAPSE_SMALL_TRIANGLES_H
#include <Eigen/Dense>
namespace igl
{
  /// Given a triangle mesh (V,F) compute a new mesh (VV,FF) which contains the
  /// original faces and vertices of (V,F) except any small triangles have been
  /// removed via collapse.
  ///
  /// We are *not* following the rules in "Mesh Optimization" [Hoppe et al]
  /// Section 4.2. But for our purposes we don't care about this criteria.
  ///
  /// @param[in]  V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle indices into V
  /// @param[in] eps  epsilon for smallest allowed area treated as fraction of squared bounding box
  ///     diagonal
  /// @param[out] FF  #FF by 3 list of triangle indices into V
  ///
  ///
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedFF>
  void collapse_small_triangles(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const double eps,
    Eigen::PlainObjectBase<DerivedFF> & FF);
}

#ifndef IGL_STATIC_LIBRARY
#  include "collapse_small_triangles.cpp"
#endif
  
#endif
