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
#include "igl_inline.h"


namespace igl
{
  /// Fake halfedge for fast and easy navigation
  /// on triangle meshes with vertex_triangle_adjacency and
  /// triangle_triangle adjacency
  ///
  /// Note: this is different to classical Half Edge data structure.
  ///    Instead, it follows cell-tuple in [Brisson, 1989]
  ///    "Representing geometric structures in d dimensions: topology and order."
  ///    This class can achieve local navigation similar to half edge in OpenMesh
  ///    But the logic behind each atom operation is different.
  ///    So this should be more properly called TriangleTupleIterator.
  ///
  /// Each tuple contains information on (face, edge, vertex)
  ///    and encoded by (face, edge \in {0,1,2}, bool reverse)
  template <
    typename DerivedF,
    typename DerivedFF,
    typename DerivedFFi>
  class HalfEdgeIterator
  {
  public:
    /// Init the HalfEdgeIterator by specifying Face,Edge Index and Orientation
    ///
    /// @param[in] F #F by 3 list of "faces"
    /// @param[in] FF #F by 3 list of triangle-triangle adjacency.
    /// @param[in] FFi #F by 3 list of FF inverse. For FF and FFi, refer to
    ///        "triangle_triangle_adjacency.h"
    /// @param[in] _fi index of the selected face
    /// @param[in] _ii index of the selected face
    /// @param[in] _reverse orientation of the selected face
    IGL_INLINE HalfEdgeIterator(
        const Eigen::MatrixBase<DerivedF>& _F,
        const Eigen::MatrixBase<DerivedFF>& _FF,
        const Eigen::MatrixBase<DerivedFFi>& _FFi,
        int _fi,
        int _ei,
        bool _reverse = false
        );

    /// Change Face
    IGL_INLINE void flipF();

    /// Change Edge
    IGL_INLINE void flipE();

    /// Change Vertex
    IGL_INLINE void flipV();

    /// Determine if on border.
    /// @returns true if the current edge is on the border
    IGL_INLINE bool isBorder();

    /// Change to next edge skipping the border
    ///      _________
    ///     ╱╲ c | b ╱╲
    ///    ╱  ╲  |  ╱  ╲
    ///   ╱ d  ╲ | ╱ a  ╲
    ///  ╱______╲|╱______╲
    ///          v
    /// In this example, if a and d are of-border and the pos is iterating
    /// counterclockwise, this method iterate through the faces incident on vertex
    /// v,
    /// producing the sequence a, b, c, d, a, b, c, ...
    ///
    /// @returns true if the next edge is not on the border
    IGL_INLINE bool NextFE();

    /// Get vertex index
    /// @return vertex index
    IGL_INLINE int Vi();

    /// Get face index
    /// @return face index
    IGL_INLINE int Fi();

    /// Get edge index
    /// @return edge index
    IGL_INLINE int Ei();

    /// Check if two HalfEdgeIterator are the same
    /// @return true if two HalfEdgeIterator are the same
    IGL_INLINE bool operator==(HalfEdgeIterator& p2);

  private:
    int fi;
    int ei;
    bool reverse;

    // All the same type? This is likely to break.
    const Eigen::MatrixBase<DerivedF> & F;
    const Eigen::MatrixBase<DerivedFF> & FF;
    const Eigen::MatrixBase<DerivedFFi> & FFi;
  };

}

#ifndef IGL_STATIC_LIBRARY
#  include "HalfEdgeIterator.cpp"
#endif

#endif
