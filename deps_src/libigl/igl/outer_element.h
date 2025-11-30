// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Qingan Zhou <qnzhou@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OUTER_ELEMENT_H
#define IGL_OUTER_ELEMENT_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Find a vertex that is reachable from infinite without crossing any faces.
  /// Such vertex is called "outer vertex."
  ///
  /// Precondition: The input mesh must have all self-intersection resolved and
  /// no duplicated vertices.  See cgal::remesh_self_intersections.h for how to
  /// obtain such input.
  ///
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle indices into V
  /// @param[in] I  #I list of facets to consider
  /// @param[out] v_index  index of outer vertex
  /// @param[out] A  #A list of facets incident to the outer vertex
  ///
  /// \fileinfo 
  template <
      typename DerivedV,
      typename DerivedF,
      typename DerivedI,
      typename IndexType,
      typename DerivedA
      >
  IGL_INLINE void outer_vertex(
          const Eigen::MatrixBase<DerivedV> & V,
          const Eigen::MatrixBase<DerivedF> & F,
          const Eigen::MatrixBase<DerivedI> & I,
          IndexType & v_index,
          Eigen::PlainObjectBase<DerivedA> & A);
  /// Find an edge that is reachable from infinity without crossing any faces.
  /// Such edge is called "outer edge."
  ///
  /// Precondition: The input mesh must have all self-intersection resolved and
  /// no duplicated vertices.  The correctness of the output depends on the fact
  /// that there is no edge overlap.  See cgal::remesh_self_intersections.h for
  /// how to obtain such input.
  ///
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle indices into V
  /// @param[in] I  #I list of facets to consider
  /// @param[out] v1 index of the first end point of outer edge
  /// @param[out] v2 index of the second end point of outer edge
  /// @param[out] A  #A list of facets incident to the outer edge
  ///
  /// \fileinfo 
  template<
      typename DerivedV,
      typename DerivedF,
      typename DerivedI,
      typename IndexType,
      typename DerivedA
      >
  IGL_INLINE void outer_edge(
          const Eigen::MatrixBase<DerivedV> & V,
          const Eigen::MatrixBase<DerivedF> & F,
          const Eigen::MatrixBase<DerivedI> & I,
          IndexType & v1,
          IndexType & v2,
          Eigen::PlainObjectBase<DerivedA> & A);
  /// Find a facet that is reachable from infinity without crossing any faces.
  /// Such facet is called "outer facet."
  ///
  /// Precondition: The input mesh must have all self-intersection resolved.  I.e
  /// there is no duplicated vertices, no overlapping edge and no intersecting
  /// faces (the only exception is there could be topologically duplicated faces).
  /// See cgal::remesh_self_intersections.h for how to obtain such input.
  ///
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle indices into V
  /// @param[in] N  #N by 3 list of face normals
  /// @param[in] I  #I list of facets to consider
  /// @param[out] f  Index of the outer facet.
  /// @param[out] flipped  true iff the normal of f points inwards.
  ///
  /// \fileinfo
  template<
      typename DerivedV,
      typename DerivedF,
      typename DerivedN,
      typename DerivedI,
      typename IndexType
      >
  IGL_INLINE void outer_facet(
          const Eigen::MatrixBase<DerivedV> & V,
          const Eigen::MatrixBase<DerivedF> & F,
          const Eigen::MatrixBase<DerivedN> & N,
          const Eigen::MatrixBase<DerivedI> & I,
          IndexType & f,
          bool & flipped);
}

#ifndef IGL_STATIC_LIBRARY
#  include "outer_element.cpp"
#endif
#endif
