// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_REMESH_SELF_INTERSECTIONS_H
#define IGL_COPYLEFT_CGAL_REMESH_SELF_INTERSECTIONS_H
#include "../../igl_inline.h"
#include "RemeshSelfIntersectionsParam.h"

#include <Eigen/Dense>
  
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Given a triangle mesh (V,F) compute a new mesh (VV,FF) which is the same
      // as (V,F) except that any self-intersecting triangles in (V,F) have been
      // subdivided (new vertices and face created) so that the self-intersection
      // contour lies exactly on edges in (VV,FF). New vertices will appear in
      // original faces or on original edges. New vertices on edges are "merged"
      // only across original faces sharing that edge. This means that if the input
      // triangle mesh is a closed manifold the output will be too.
      //
      // Inputs:
      //   V  #V by 3 list of vertex positions
      //   F  #F by 3 list of triangle indices into V
      //   params  struct of optional parameters
      // Outputs:
      //   VV  #VV by 3 list of vertex positions
      //   FF  #FF by 3 list of triangle indices into VV
      //   IF  #intersecting face pairs by 2  list of intersecting face pairs,
      //     indexing F
      //   J  #FF list of indices into F denoting birth triangle
      //   IM  #VV list of indices into VV of unique vertices.
      //
      // Known bugs: If an existing edge in (V,F) lies exactly on another face then
      // any resulting additional vertices along that edge may not get properly
      // connected so that the output mesh has the same global topology. This is
      // because 
      //
      // Example:
      //     // resolve intersections
      //     igl::copyleft::cgal::remesh_self_intersections(V,F,params,VV,FF,IF,J,IM);
      //     // _apply_ duplicate vertex mapping IM to FF
      //     for_each(FF.data(),FF.data()+FF.size(),[&IM](int & a){a=IM(a);});
      //     // remove any vertices now unreferenced after duplicate mapping.
      //     igl::remove_unreferenced(VV,FF,SV,SF,UIM);
      //     // Now (SV,SF) is ready to extract outer hull
      //     igl::copyleft::cgal::outer_hull(SV,SF,G,J,flip);
      //
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedVV,
        typename DerivedFF,
        typename DerivedIF,
        typename DerivedJ,
        typename DerivedIM>
      IGL_INLINE void remesh_self_intersections(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        const RemeshSelfIntersectionsParam & params,
        Eigen::PlainObjectBase<DerivedVV> & VV,
        Eigen::PlainObjectBase<DerivedFF> & FF,
        Eigen::PlainObjectBase<DerivedIF> & IF,
        Eigen::PlainObjectBase<DerivedJ> & J,
        Eigen::PlainObjectBase<DerivedIM> & IM);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "remesh_self_intersections.cpp"
#endif
  
#endif
