// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_ORDER_FACETS_AROUND_EDGES_H
#define IGL_COPYLEFT_CGAL_ORDER_FACETS_AROUND_EDGES_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <vector>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// For each undirected edge, sort its adjacent faces.  Assuming the
      /// undirected edge is (s, d).  Sort the adjacent faces clockwise around the
      /// axis (d - s), i.e. left-hand rule.  An adjacent face is consistently
      /// oriented if it contains (d, s) as a directed edge.
      ///
      /// For overlapping faces, break the tie using signed face index, smaller
      /// signed index comes before the larger signed index.  Signed index is
      /// computed as (consistent? 1:-1) * index.
      ///
      /// @param[in] V    #V by 3 list of vertices.
      /// @param[in] F    #F by 3 list of faces
      /// @param[in] N    #F by 3 list of face normals.
      /// @param[in] uE    #uE by 2 list of vertex_indices, represents undirected edges.
      /// @param[in] uE2E  #uE list of lists that maps uE to E. (a one-to-many map)
      /// @param[out] uE2oE #uE list of lists that maps uE to an ordered list of E. (a
      ///        one-to-many map)
      /// @param[out] uE2C  #uE list of lists of bools indicates whether each face in
      ///         uE2oE[i] is consistently orientated as the ordering.
      ///
      template<
          typename DerivedV,
          typename DerivedF,
          typename DerivedN,
          typename DeriveduE,
          typename uE2EType,
          typename uE2oEType,
          typename uE2CType >
      IGL_INLINE
      typename std::enable_if<!std::is_same<typename DerivedV::Scalar,
      typename CGAL::Exact_predicates_exact_constructions_kernel::FT>::value, void>::type
      order_facets_around_edges(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const Eigen::MatrixBase<DerivedN>& N,
        const Eigen::MatrixBase<DeriveduE>& uE,
        const std::vector<std::vector<uE2EType> >& uE2E,
        std::vector<std::vector<uE2oEType> >& uE2oE,
        std::vector<std::vector<uE2CType > >& uE2C );
      /// \overload
      template<
          typename DerivedV,
          typename DerivedF,
          typename DerivedN,
          typename DeriveduE,
          typename uE2EType,
          typename uE2oEType,
          typename uE2CType >
      IGL_INLINE 
      typename std::enable_if<std::is_same<typename DerivedV::Scalar,
      typename CGAL::Exact_predicates_exact_constructions_kernel::FT>::value, void>::type
      order_facets_around_edges(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const Eigen::MatrixBase<DerivedN>& N,
        const Eigen::MatrixBase<DeriveduE>& uE,
        const std::vector<std::vector<uE2EType> >& uE2E,
        std::vector<std::vector<uE2oEType> >& uE2oE,
        std::vector<std::vector<uE2CType > >& uE2C );
      /// \overload
      /// \brief Order faces around each edge. Only exact predicate is used in
      /// the algorithm.  Normal is not needed.
      template<
          typename DerivedV,
          typename DerivedF,
          typename DeriveduE,
          typename uE2EType,
          typename uE2oEType,
          typename uE2CType >
      IGL_INLINE void order_facets_around_edges(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const Eigen::MatrixBase<DeriveduE>& uE,
        const std::vector<std::vector<uE2EType> >& uE2E,
        std::vector<std::vector<uE2oEType> >& uE2oE,
        std::vector<std::vector<uE2CType > >& uE2C );
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#include "order_facets_around_edges.cpp"
#endif

#endif
