// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EDGE_MIDPOINTS_H
#define IGL_EDGE_MIDPOINTS_H
#include "igl_inline.h"

#include <Eigen/Dense>
namespace igl
{
  /// Computes the midpoints of edges in a triangle mesh.
  ///
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle indices
  /// @param[in] E #F by 3 a mapping from each halfedge to each edge
  /// @param[in] oE #F by 3 the orientation (e.g., -1 or 1) of each halfedge
  ///    compared to the orientation of the actual edge, as computed with
  ///    orient_halfedges. will be computed if not provided.
  /// @param[out] mps |HE| list of edge midpoints
  ///
  /// \see orient_halfedges
  template<typename DerivedV,typename DerivedF,typename DerivedE,
  typename DerivedoE, typename Derivedmps>
  IGL_INLINE void edge_midpoints(
    const Eigen::MatrixBase<DerivedV> &V,
    const Eigen::MatrixBase<DerivedF> &F,
    const Eigen::MatrixBase<DerivedE> &E,
    const Eigen::MatrixBase<DerivedoE> &oE,
    Eigen::PlainObjectBase<Derivedmps> &mps);
}

#ifndef IGL_STATIC_LIBRARY
#  include "edge_midpoints.cpp"
#endif

#endif
