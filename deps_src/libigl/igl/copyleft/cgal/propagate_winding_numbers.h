// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#ifndef IGL_COPYLEFT_CGAL_PROPAGATE_WINDING_NUMBERS_H
#define IGL_COPYLEFT_CGAL_PROPAGATE_WINDING_NUMBERS_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

// The following methods compute the winding number on each side of each facet
// or patch of a 3D mesh.  The input mesh is valid if it splits the ambient
// space, R^3, into subspaces with constant integer winding numbers.  That is
// the input mesh should be closed and for each directed edge the number of
// clockwise facing facets should equal the number of counterclockwise facing
// facets.

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // TODO: This shouldn't need to be in igl::copyleft::cgal, it should
      // instead take as input an index of the ambient cell and the winding
      // number vector there.
      //
      // Compute winding number on each side of the face.  The input mesh
      // could contain multiple connected components.  The input mesh must
      // represent the boundary of a valid 3D volume, which means it is
      // closed, consistently oriented and induces integer winding numbers.
      //
      // Inputs:
      //   V  #V by 3 list of vertex positions.
      //   F  #F by 3 list of triangle indices into V.
      //   labels  #F list of facet labels ranging from 0 to k-1.
      // Output:
      //   W  #F by k*2 list of winding numbers.  ``W(i,j*2)`` is the winding
      //      number on the positive side of facet ``i`` with respect to the
      //      facets labeled ``j``.  Similarly, ``W(i,j*2+1)`` is the winding
      //      number on the negative side of facet ``i`` with respect to the
      //      facets labeled ``j``.
      // Returns true iff the input induces a piecewise-constant winding number
      //   field.
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedL,
        typename DerivedW>
      IGL_INLINE bool propagate_winding_numbers(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedL>& labels,
        Eigen::PlainObjectBase<DerivedW>& W);

      // Inputs:
      //   V  #V by 3 list of vertex positions.
      //   F  #F by 3 list of triangle indices into V.
      //   uE    #uE by 2 list of vertex_indices, represents undirected edges.
      //   uE2E  #uE list of lists that maps uE to E. (a one-to-many map)
      //   num_patches  number of patches
      //   P  #F list of patch ids.
      //   num_cells    number of cells
      //   C  #P by 2 list of cell ids on each side of each patch.
      //   labels  #F list of facet labels ranging from 0 to k-1.
      // Output:
      //   W  #F by k*2 list of winding numbers.  ``W(i,j*2)`` is the winding
      //      number on the positive side of facet ``i`` with respect to the
      //      facets labeled ``j``.  Similarly, ``W(i,j*2+1)`` is the winding
      //      number on the negative side of facet ``i`` with respect to the
      //      facets labeled ``j``.
      template<
        typename DerivedV,
        typename DerivedF,
        typename DeriveduE,
        typename uE2EType,
        typename DerivedP,
        typename DerivedC,
        typename DerivedL,
        typename DerivedW>
      IGL_INLINE bool propagate_winding_numbers(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DeriveduE>& uE,
        const std::vector<std::vector<uE2EType> >& uE2E,
        const size_t num_patches,
        const Eigen::PlainObjectBase<DerivedP>& P,
        const size_t num_cells,
        const Eigen::PlainObjectBase<DerivedC>& C,
        const Eigen::PlainObjectBase<DerivedL>& labels,
        Eigen::PlainObjectBase<DerivedW>& W);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "propagate_winding_numbers.cpp"
#endif
#endif
