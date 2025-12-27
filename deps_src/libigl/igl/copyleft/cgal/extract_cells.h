// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#ifndef IGL_COPYLEFT_CGAL_EXTRACT_CELLS
#define IGL_COPYLEFT_CGAL_EXTRACT_CELLS

#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl {
  namespace copyleft
  {
    namespace cgal
    {
      // Extract connected 3D space partitioned by mesh (V, F).
      //
      // Inputs:
      //   V  #V by 3 array of vertices.
      //   F  #F by 3 array of faces.
      //
      // Output:
      //   cells  #F by 2 array of cell indices.  cells(i,0) represents the
      //          cell index on the positive side of face i, and cells(i,1)
      //          represents cell index of the negqtive side.
      //          By convension cell with index 0 is the infinite cell.
      // Returns the number of cells
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedC >
      IGL_INLINE size_t extract_cells(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        Eigen::PlainObjectBase<DerivedC>& cells);

      // Extract connected 3D space partitioned by mesh (V, F).
      //
      // Inputs:
      //   V  #V by 3 array of vertices.
      //   F  #F by 3 array of faces.
      //   P  #F list of patch indices.
      //   E  #E by 2 array of vertex indices, one edge per row.
      //  uE    #uE by 2 list of vertex_indices, represents undirected edges.
      //  uE2E  #uE list of lists that maps uE to E. (a one-to-many map)
      //  EMAP  #F*3 list of indices into uE.
      //
      // Output:
      //   cells  #P by 2 array of cell indices.  cells(i,0) represents the
      //          cell index on the positive side of patch i, and cells(i,1)
      //          represents cell index of the negqtive side.
      //          By convension cell with index 0 is the infinite cell.
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedP,
        typename DerivedE,
        typename DeriveduE,
        typename uE2EType,
        typename DerivedEMAP,
        typename DerivedC >
      IGL_INLINE size_t extract_cells(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedP>& P,
        const Eigen::PlainObjectBase<DerivedE>& E,
        const Eigen::PlainObjectBase<DeriveduE>& uE,
        const std::vector<std::vector<uE2EType> >& uE2E,
        const Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
        Eigen::PlainObjectBase<DerivedC>& cells);

      // Extract connected 3D space partitioned by mesh (V,F) composed of
      // **possibly multiple components** (the name of this function is
      // dubious).
      //
      // Inputs:
      //   V  #V by 3 array of vertices.
      //   F  #F by 3 array of faces.
      //   P  #F list of patch indices.
      //   E  #E by 2 array of vertex indices, one edge per row.
      //  uE    #uE by 2 list of vertex_indices, represents undirected edges.
      //  uE2E  #uE list of lists that maps uE to E. (a one-to-many map)
      //  EMAP  #F*3 list of indices into uE.
      // Output:
      //  cells  #P by 2 array of cell indices.  cells(i,0) represents the
      //    cell index on the positive side of patch i, and cells(i,1)
      //    represents cell index of the negative side.
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedP,
        typename DeriveduE,
        typename uE2EType,
        typename DerivedEMAP,
        typename DerivedC >
      IGL_INLINE size_t extract_cells_single_component(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedP>& P,
        const Eigen::PlainObjectBase<DeriveduE>& uE,
        const std::vector<std::vector<uE2EType> >& uE2E,
        const Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
        Eigen::PlainObjectBase<DerivedC>& cells);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "extract_cells.cpp"
#endif
#endif
