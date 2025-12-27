// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_PROJECTED_CDT_H
#define IGL_COPYLEFT_CGAL_PROJECTED_CDT_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <CGAL/Plane_3.h>
#include <CGAL/Point_3.h>
#include <CGAL/Object.h>
#include <vector>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Given a list of objects (e.g., resulting from intersecting a triangle
      // with many other triangles), construct a constrained Delaunay
      // triangulation on a given plane (P), by inersting constraints for each
      // object projected onto that plane.
      //
      // Inputs:
      //   objects  list of objects. This should lie on the given plane (P),
      //     otherwise they are added to the cdt _after_ their non-trivial
      //     projection
      //   P  plane upon which all objects lie and upon which the CDT is
      //     conducted
      // Outputs:
      //   vertices  list of vertices of the CDT mesh _back on the 3D plane_
      //   faces  list of list of triangle indices into vertices
      //
      template <typename Kernel, typename Index>
      IGL_INLINE void projected_cdt(
        const std::vector<CGAL::Object> & objects,
        const CGAL::Plane_3<Kernel> & P,
        std::vector<CGAL::Point_3<Kernel> >& vertices,
        std::vector<std::vector<Index> >& faces);
      // Outputs:
      //   V  #V by 3 list of vertices of the CDT mesh _back on the 3D plane_,
      //     **cast** from the number type of Kernel to the number type of
      //     DerivedV
      //   F  #F by 3 list of triangle indices into V
      template < typename Kernel, typename DerivedV, typename DerivedF>
      IGL_INLINE void projected_cdt(
        const std::vector<CGAL::Object> & objects,
        const CGAL::Plane_3<Kernel> & P,
        Eigen::PlainObjectBase<DerivedV> & V,
        Eigen::PlainObjectBase<DerivedF> & F);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "projected_cdt.cpp"
#endif
#endif
