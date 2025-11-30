// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_PARALLEL_TRANSPORT_ANGLE
#define IGL_PARALLEL_TRANSPORT_ANGLE
#include "igl_inline.h"

#include <Eigen/Core>
#include <vector>

namespace igl {
  /// Given the per-face local bases computed via igl::local_basis, this function
  /// computes the angle between the two reference frames across each edge.
  /// Any two vectors across the edge whose 2D representation only differs by
  /// this angle are considered to be parallel.
  ///
  /// @param[in] V   #V by 3 list of mesh vertex coordinates
  /// @param[in] F   #F by 3 list of mesh faces (must be triangles)
  /// @param[in] FN  #F by 3 list of face normals
  /// @param[in] E2F #E by 2 list of the edge-to-face relation (e.g. computed
  ///                via igl::edge_topology)
  /// @param[in] F2E #F by 3 list of the face-to-edge relation (e.g. computed
  /// @param[out] K  #E by 1 list of the parallel transport angles (zero
  ///                   for all boundary edges)
  ///
  template <typename DerivedV, typename DerivedF, typename DerivedK>
  IGL_INLINE void parallel_transport_angles(
    const Eigen::MatrixBase<DerivedV>&V,
    const Eigen::MatrixBase<DerivedF>&F,
    const Eigen::MatrixBase<DerivedV>&FN,
    const Eigen::MatrixXi &E2F,
    const Eigen::MatrixXi &F2E,
    Eigen::PlainObjectBase<DerivedK>&K);

};


#ifndef IGL_STATIC_LIBRARY
#include "parallel_transport_angles.cpp"
#endif


#endif /* defined(IGL_PARALLEL_TRANSPORT_ANGLE) */
