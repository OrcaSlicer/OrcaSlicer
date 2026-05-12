// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2022 Alec Jacobson <alecjacobson@gmail.com>
// Copyright (C) 2016 Qingan Zhou <qnzhou@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_PREDICATES_DELAUNAY_TRIANGULATION_H
#define IGL_PREDICATES_DELAUNAY_TRIANGULATION_H

#include "../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace predicates
  {
    /// Given a set of points in 2D, return a Delaunay triangulation of these
    /// points using predicates.
    ///
    /// @param[in] V  #V by 2 list of vertex positions
    /// @param[out] F  #F by 3 of faces in Delaunay triangulation.
    template<
      typename DerivedV,
      typename DerivedF
      >
    IGL_INLINE void delaunay_triangulation(
        const Eigen::MatrixBase<DerivedV>& V,
        Eigen::PlainObjectBase<DerivedF>& F);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "delaunay_triangulation.cpp"
#endif
#endif

