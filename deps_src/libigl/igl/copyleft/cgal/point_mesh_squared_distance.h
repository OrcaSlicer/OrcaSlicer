// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_POINT_MESH_SQUARED_DISTANCE_H
#define IGL_COPYLEFT_CGAL_POINT_MESH_SQUARED_DISTANCE_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>
#include "CGAL_includes.hpp"
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Compute distances from a set of points P to a triangle mesh (V,F)
      //
      // Templates:
      //   Kernal  CGAL computation and construction kernel (e.g.
      //     CGAL::Simple_cartesian<double>)
      // Inputs:
      //   P  #P by 3 list of query point positions
      //   V  #V by 3 list of vertex positions
      //   F  #F by 3 list of triangle indices
      // Outputs:
      //   sqrD  #P list of smallest squared distances
      //   I  #P list of facet indices corresponding to smallest distances
      //   C  #P by 3 list of closest points
      //
      // Known bugs: This only computes distances to triangles. So unreferenced
      // vertices and degenerate triangles (segments) are ignored.
      template <
        typename Kernel,
        typename DerivedP,
        typename DerivedV,
        typename DerivedF,
        typename DerivedsqrD,
        typename DerivedI,
        typename DerivedC>
      IGL_INLINE void point_mesh_squared_distance(
        const Eigen::PlainObjectBase<DerivedP> & P,
        const Eigen::PlainObjectBase<DerivedV> & V,
        const Eigen::PlainObjectBase<DerivedF> & F,
              Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
              Eigen::PlainObjectBase<DerivedI> & I,
              Eigen::PlainObjectBase<DerivedC> & C);
      // Probably can do this in a way that we don't pass around `tree` and `T`
      //
      // Outputs:
      //   tree  CGAL's AABB tree
      //   T  list of CGAL triangles in order of F (for determining which was found
      //     in computation)
      template <
        typename Kernel,
        typename DerivedV,
        typename DerivedF
        >
      IGL_INLINE void point_mesh_squared_distance_precompute(
        const Eigen::PlainObjectBase<DerivedV> & V,
        const Eigen::PlainObjectBase<DerivedF> & F,
        CGAL::AABB_tree<
          CGAL::AABB_traits<Kernel, 
            CGAL::AABB_triangle_primitive<Kernel, 
              typename std::vector<CGAL::Triangle_3<Kernel> >::iterator
            >
          >
        > & tree,
        std::vector<CGAL::Triangle_3<Kernel> > & T);
      // Inputs:
      //  see above
      // Outputs:
      //  see above
      template <
        typename Kernel,
        typename DerivedP,
        typename DerivedsqrD,
        typename DerivedI,
        typename DerivedC>
      IGL_INLINE void point_mesh_squared_distance(
        const Eigen::PlainObjectBase<DerivedP> & P,
        const CGAL::AABB_tree<
          CGAL::AABB_traits<Kernel, 
            CGAL::AABB_triangle_primitive<Kernel, 
              typename std::vector<CGAL::Triangle_3<Kernel> >::iterator
            >
          >
        > & tree,
        const std::vector<CGAL::Triangle_3<Kernel> > & T,
        Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
        Eigen::PlainObjectBase<DerivedI> & I,
        Eigen::PlainObjectBase<DerivedC> & C);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "point_mesh_squared_distance.cpp"
#endif

#endif
