// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2019 Hanxiao Shen <hanxiao@cs.nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_PREDICATES_EAR_CLIPPING_H
#define IGL_PREDICATES_EAR_CLIPPING_H

#include <Eigen/Core>
#include "../igl_inline.h"

namespace igl
{
  namespace predicates
  {
    /// Implementation of ear clipping triangulation algorithm for a 2D polygon.
    /// https://www.geometrictools.com/Documentation/TriangulationByEarClipping.pdf
    /// If the polygon is simple and oriented counter-clockwise, all vertices
    /// will be clipped and the result mesh is (P,eF) Otherwise, the function
    /// will try to clip as many ears as possible.
    ///
    /// @param[in] P : n*2, size n 2D polygon 
    /// @param[in] RT: n*1, preserved vertices (do not clip) marked as 1, otherwise 0
    /// @param[out] eF: clipped ears, in original index of P
    /// @param[out] I : size #nP vector, maps index from nP to P, e.g. nP's ith vertex is origianlly I(i) in P
    ///
    /// \pre To result in a proper mesh, P should be oriented counter-clockwise
    /// with no self-intersections.
    ///
    /// \note This implementation does not handle polygons with holes.
    ///
    /// \bug https://github.com/libigl/libigl/issues/1563
    template <
      typename DerivedP, 
      typename DerivedRT,
      typename DerivedF, 
      typename DerivedI>
    IGL_INLINE void ear_clipping(
      const Eigen::MatrixBase<DerivedP>& P,
      const Eigen::MatrixBase<DerivedRT>& RT,
      Eigen::PlainObjectBase<DerivedF>& eF, 
      Eigen::PlainObjectBase<DerivedI>& I);
    /// \overload
    /// \brief Reverses P if necessary. Orientation of output will match input
    /// \return true if mesh is proper (should correspond to input being a
    /// simple polygon in either orientation), false otherwise.
    template <typename DerivedP, typename DerivedF>
    IGL_INLINE bool ear_clipping(
      const Eigen::MatrixBase<DerivedP>& P,
      Eigen::PlainObjectBase<DerivedF>& eF);

  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "ear_clipping.cpp"
#endif


#endif
