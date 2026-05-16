// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EXPLODED_VIEW_H
#define IGL_EXPLODED_VIEW_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Given a tet-mesh, create a trivial surface mesh (4 triangles per tet) with
  /// each tet scaled individually and translated outward from the mesh's
  /// centroid, creating an exploded-view visualization.
  ///
  /// @param[in] V  #V by 3 list of tet mesh vertex positions
  /// @param[in] T  #T by 4 list of tet mesh indices into rows of V
  /// @param[in] s  amount to scale each tet indvidually, typically (0,1]
  /// @param[in] t  amount to scale away from mesh's centroid, typically >=1
  /// @param[out] EV  #T*4 by 3 list of output mesh vertex positions
  /// @param[out] EF  #T*4 by 3 list of output triangle indices into rows of EV
  /// @param[out] I  #EV list of indices into V revealing birth parent
  /// @param[out] J  #EF list of indices into F revealing birth parent
  template <
    typename DerivedV,
    typename DerivedT,
    typename DerivedEV,
    typename DerivedEF,
    typename DerivedI,
    typename DerivedJ>
  IGL_INLINE void exploded_view(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedT> & T,
    const typename DerivedV::Scalar s,
    const typename DerivedV::Scalar t,
    Eigen::PlainObjectBase<DerivedEV> & EV,
    Eigen::PlainObjectBase<DerivedEF> & EF,
    Eigen::PlainObjectBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedJ> & J);
}

#ifndef IGL_STATIC_LIBRARY
#  include "exploded_view.cpp"
#endif

#endif
