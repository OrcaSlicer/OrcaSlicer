// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ORIENT_HALFEDGES_H
#define IGL_ORIENT_HALFEDGES_H

#include "igl_inline.h"
#include <Eigen/Core>


namespace igl
{
  /// Orients halfedges for a triangle mesh, assigning them to a unique edge.
  ///
  ///  @param[in] F #F by 3 input mesh connectivity
  ///  @param[out] E  #F by 3 a mapping from each halfedge to each edge
  ///  @param[out] oE #F by 3 the orientation (e.g., -1 or 1) of each halfedge compared to
  ///    the orientation of the actual edge. Every edge appears positively oriented
  ///    exactly once.
  ///
  /// \see unique_simplices
  template <typename DerivedF, typename DerivedE, typename DerivedOE>
  IGL_INLINE void orient_halfedges(
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedE>& E,
    Eigen::PlainObjectBase<DerivedOE>& oE);

}


#ifndef IGL_STATIC_LIBRARY
#  include "orient_halfedges.cpp"
#endif

#endif
