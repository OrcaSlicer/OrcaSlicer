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
  // Find a vertex that is reachable from infinite without crossing any faces.
  // Such vertex is called "outer vertex."
  //
  // Precondition: The input mesh must have all self-intersection resolved and
  // no duplicated vertices.  See cgal::remesh_self_intersections.h for how to
  // obtain such input.
  //
  // Inputs:
  //   V  #V by 3 list of vertex positions
  //   F  #F by 3 list of triangle indices into V
  //   I  #I list of facets to consider
  // Outputs:
  //   v_index  index of outer vertex
  //   A  #A list of facets incident to the outer vertex
  template <
      typename DerivedV,
      typename DerivedF,
      typename DerivedI,
      typename IndexType,
      typename DerivedA
      >
  IGL_INLINE void outer_vertex(
          const Eigen::PlainObjectBase<DerivedV> & V,
          const Eigen::PlainObjectBase<DerivedF> & F,
          const Eigen::PlainObjectBase<DerivedI> & I,
          IndexType & v_index,
          Eigen::PlainObjectBase<DerivedA> & A);


  // Find an edge that is reachable from infinity without crossing any faces.
  // Such edge is called "outer edge."
  //
  // Precondition: The input mesh must have all self-intersection resolved and
  // no duplicated vertices.  The correctness of the output depends on the fact
  // that there is no edge overlap.  See cgal::remesh_self_intersections.h for
  // how to obtain such input.
  //
  // Inputs:
  //   V  #V by 3 list of vertex positions
  //   F  #F by 3 list of triangle indices into V
  //   I  #I list of facets to consider
  // Outputs:
  //   v1 index of the first end point of outer edge
  //   v2 index of the second end point of outer edge
  //   A  #A list of facets incident to the outer edge
  template<
      typename DerivedV,
      typename DerivedF,
      typename DerivedI,
      typename IndexType,
      typename DerivedA
      >
  IGL_INLINE void outer_edge(
          const Eigen::PlainObjectBase<DerivedV> & V,
          const Eigen::PlainObjectBase<DerivedF> & F,
          const Eigen::PlainObjectBase<DerivedI> & I,
          IndexType & v1,
          IndexType & v2,
          Eigen::PlainObjectBase<DerivedA> & A);


  // Find a facet that is reachable from infinity without crossing any faces.
  // Such facet is called "outer facet."
  //
  // Precondition: The input mesh must have all self-intersection resolved.  I.e
  // there is no duplicated vertices, no overlapping edge and no intersecting
  // faces (the only exception is there could be topologically duplicated faces).
  // See cgal::remesh_self_intersections.h for how to obtain such input.
  //
  // Inputs:
  //   V  #V by 3 list of vertex positions
  //   F  #F by 3 list of triangle indices into V
  //   N  #N by 3 list of face normals
  //   I  #I list of facets to consider
  // Outputs:
  //   f  Index of the outer facet.
  //   flipped  true iff the normal of f points inwards.
  template<
      typename DerivedV,
      typename DerivedF,
      typename DerivedN,
      typename DerivedI,
      typename IndexType
      >
  IGL_INLINE void outer_facet(
          const Eigen::PlainObjectBase<DerivedV> & V,
          const Eigen::PlainObjectBase<DerivedF> & F,
          const Eigen::PlainObjectBase<DerivedN> & N,
          const Eigen::PlainObjectBase<DerivedI> & I,
          IndexType & f,
          bool & flipped);
}

#ifndef IGL_STATIC_LIBRARY
#  include "outer_element.cpp"
#endif
#endif
