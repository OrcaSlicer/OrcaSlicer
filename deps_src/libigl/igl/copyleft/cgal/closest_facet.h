// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#ifndef IGL_COPYLET_CGAL_CLOSEST_FACET_H
#define IGL_COPYLET_CGAL_CLOSEST_FACET_H

#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

#include <CGAL/AABB_tree.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_triangle_primitive.h>
#include <CGAL/intersections.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Determine the closest facet for each of the input points.
      ///
      /// @param[in] V  #V by 3 array of vertices.
      /// @param[in] F  #F by 3 array of faces.
      /// @param[in] I  #I list of triangle indices to consider.
      /// @param[in] P  #P by 3 array of query points.
      /// @param[in] EMAP  #F*3 list of indices into uE.
      /// @param[in] uEC  #uE+1 list of cumsums of directed edges sharing each unique edge
      /// @param[in] uEE  #E list of indices into E (see `igl::unique_edge_map`)
      /// @param[in] VF  #V list of lists of incident faces (adjacency list)
      /// @param[in] VFi  #V list of lists of index of incidence within incident faces listed in VF
      /// @param[in] tree  AABB containing triangles of (V,F(I,:))
      /// @param[in] triangles  #I list of cgal triangles
      /// @param[in] in_I  #F list of whether in submesh
      /// @param[out] R  #P list of closest facet indices.
      /// @param[out] S  #P list of bools indicating on which side of the closest facet
      ///      each query point lies.
      ///
      /// \note The use of `size_t` here is a bad idea. These should just be int
      /// to avoid nonsense with windows.
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedI,
        typename DerivedP,
        typename DerivedEMAP,
        typename DeriveduEC,
        typename DeriveduEE,
        typename Kernel,
        typename DerivedR,
        typename DerivedS >
      IGL_INLINE void closest_facet(
          const Eigen::MatrixBase<DerivedV>& V,
          const Eigen::MatrixBase<DerivedF>& F,
          const Eigen::MatrixBase<DerivedI>& I,
          const Eigen::MatrixBase<DerivedP>& P,
          const Eigen::MatrixBase<DerivedEMAP>& EMAP,
          const Eigen::MatrixBase<DeriveduEC>& uEC,
          const Eigen::MatrixBase<DeriveduEE>& uEE,
          const std::vector<std::vector<size_t> > & VF,
          const std::vector<std::vector<size_t> > & VFi,
          const CGAL::AABB_tree<
            CGAL::AABB_traits<
              Kernel, 
              CGAL::AABB_triangle_primitive<
                Kernel, typename std::vector<
                  typename Kernel::Triangle_3 >::iterator > > > & tree,
          const std::vector<typename Kernel::Triangle_3 > & triangles,
          const std::vector<bool> & in_I,
          Eigen::PlainObjectBase<DerivedR>& R,
          Eigen::PlainObjectBase<DerivedS>& S);
      /// \overload
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedI,
        typename DerivedP,
        typename DerivedEMAP,
        typename DeriveduEC,
        typename DeriveduEE,
        typename DerivedR,
        typename DerivedS >
      IGL_INLINE void closest_facet(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const Eigen::MatrixBase<DerivedI>& I,
        const Eigen::MatrixBase<DerivedP>& P,
        const Eigen::MatrixBase<DerivedEMAP>& EMAP,
        const Eigen::MatrixBase<DeriveduEC>& uEC,
        const Eigen::MatrixBase<DeriveduEE>& uEE,
              Eigen::PlainObjectBase<DerivedR>& R,
              Eigen::PlainObjectBase<DerivedS>& S);
      /// \overload
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedP,
        typename DerivedEMAP,
        typename DeriveduEC,
        typename DeriveduEE,
        typename DerivedR,
        typename DerivedS >
      IGL_INLINE void closest_facet(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const Eigen::MatrixBase<DerivedP>& P,
        const Eigen::MatrixBase<DerivedEMAP>& EMAP,
        const Eigen::MatrixBase<DeriveduEC>& uEC,
        const Eigen::MatrixBase<DeriveduEE>& uEE,
        Eigen::PlainObjectBase<DerivedR>& R,
        Eigen::PlainObjectBase<DerivedS>& S);
      /// \overload
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedI,
        typename DerivedP,
        typename DerivedEMAP,
        typename DeriveduEC,
        typename DeriveduEE,
        typename Kernel,
        typename DerivedR,
        typename DerivedS >
      IGL_INLINE void closest_facet(
          const Eigen::MatrixBase<DerivedV>& V,
          const Eigen::MatrixBase<DerivedF>& F,
          const Eigen::MatrixBase<DerivedI>& I,
          const Eigen::MatrixBase<DerivedP>& P,
          const Eigen::MatrixBase<DerivedEMAP>& EMAP,
          const Eigen::MatrixBase<DeriveduEC>& uEC,
          const Eigen::MatrixBase<DeriveduEE>& uEE,
          const std::vector<std::vector<size_t> > & VF,
          const std::vector<std::vector<size_t> > & VFi,
          const CGAL::AABB_tree<
            CGAL::AABB_traits<
              Kernel, 
              CGAL::AABB_triangle_primitive<
                Kernel, typename std::vector<
                  typename Kernel::Triangle_3 >::iterator > > > & tree,
          const std::vector<typename Kernel::Triangle_3 > & triangles,
          const std::vector<bool> & in_I,
          Eigen::PlainObjectBase<DerivedR>& R,
          Eigen::PlainObjectBase<DerivedS>& S);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#include "closest_facet.cpp"
#endif
#endif
