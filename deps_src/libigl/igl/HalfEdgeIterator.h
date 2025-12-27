// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_HALFEDGEITERATOR_H
#define IGL_HALFEDGEITERATOR_H

#include <Eigen/Core>

#include <vector>
#include <igl/igl_inline.h>

// This file violates many of the libigl style guidelines.

namespace igl
{
  // HalfEdgeIterator - Fake halfedge for fast and easy navigation
  // on triangle meshes with vertex_triangle_adjacency and
  // triangle_triangle adjacency
  //
  // Note: this is different to classical Half Edge data structure.
  //    Instead, it follows cell-tuple in [Brisson, 1989]
  //    "Representing geometric structures in d dimensions: topology and order."
  //    This class can achieve local navigation similar to half edge in OpenMesh
  //    But the logic behind each atom operation is different.
  //    So this should be more properly called TriangleTupleIterator.
  //
  // Each tuple contains information on (face, edge, vertex)
  //    and encoded by (face, edge \in {0,1,2}, bool reverse)
  //
  // Inputs:
  //    F #F by 3 list of "faces"
  //    FF #F by 3 list of triangle-triangle adjacency.
  //    FFi #F by 3 list of FF inverse. For FF and FFi, refer to
  //        "triangle_triangle_adjacency.h"
  // Usages:
  //    FlipF/E/V changes solely one actual face/edge/vertex resp.
  //    NextFE iterates through one-ring of a vertex robustly.
  //
  template <
    typename DerivedF,
    typename DerivedFF,
    typename DerivedFFi>
  class HalfEdgeIterator
  {
  public:
    // Init the HalfEdgeIterator by specifying Face,Edge Index and Orientation
    IGL_INLINE HalfEdgeIterator(
        const Eigen::PlainObjectBase<DerivedF>& _F,
        const Eigen::PlainObjectBase<DerivedFF>& _FF,
        const Eigen::PlainObjectBase<DerivedFFi>& _FFi,
        int _fi,
        int _ei,
        bool _reverse = false
        );

    // Change Face
    IGL_INLINE void flipF();

    // Change Edge
    IGL_INLINE void flipE();

    // Change Vertex
    IGL_INLINE void flipV();

    IGL_INLINE bool isBorder();

    /*!
     * Returns the next edge skipping the border
     *      _________
     *     /\ c | b /\
     *    /  \  |  /  \
     *   / d  \ | / a  \
     *  /______\|/______\
     *          v
     * In this example, if a and d are of-border and the pos is iterating
     counterclockwise, this method iterate through the faces incident on vertex
     v,
     * producing the sequence a, b, c, d, a, b, c, ...
     */
    IGL_INLINE bool NextFE();

    // Get vertex index
    IGL_INLINE int Vi();

    // Get face index
    IGL_INLINE int Fi();

    // Get edge index
    IGL_INLINE int Ei();

    IGL_INLINE bool operator==(HalfEdgeIterator& p2);

  private:
    int fi;
    int ei;
    bool reverse;

    // All the same type? This is likely to break.
    const Eigen::PlainObjectBase<DerivedF> & F;
    const Eigen::PlainObjectBase<DerivedFF> & FF;
    const Eigen::PlainObjectBase<DerivedFFi> & FFi;
  };

}

#ifndef IGL_STATIC_LIBRARY
#  include "HalfEdgeIterator.cpp"
#endif

#endif
