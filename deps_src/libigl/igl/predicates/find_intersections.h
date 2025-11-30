// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2024 Alec Jacobson alecjacobson@gmail.com
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/
#ifndef IGL_FAST_FIND_INTERSECTIONS_H
#define IGL_FAST_FIND_INTERSECTIONS_H

#include "../igl_inline.h"
#include <Eigen/Core>

namespace igl 
{
  template <typename DerivedV, int DIM> class AABB;
  namespace predicates
  {
    /// Identify triangles where two meshes interesect 
    /// using AABBTree and igl::predicates::triangle_triangle_intersect.
    ///
    /// @param[in] tree1 AABB tree for the first mesh
    /// @param[in] V1  #V1 by 3 list representing vertices on the first mesh
    /// @param[in] F1  #F1 by 3 list representing triangles on the first mesh
    /// @param[in] V2  #V2 by 3 list representing vertices on the second mesh
    /// @param[in] F2  #F2 by 3 list representing triangles on the second mesh
    /// @param[out] IF #IF by 2 list of intersecting triangle pairs, so that 
    ///   F1(IF(i,0),:) intersects F2(IF(i,1),:)
    /// @param[out] CP #IF list of whether the intersection is coplanar
    ///
    /// \see copyleft::cgal::intersect_other
    template <
      typename DerivedV1,
      typename DerivedF1,
      typename DerivedV2,
      typename DerivedF2,
      typename DerivedIF,
      typename DerivedCP>
    IGL_INLINE bool find_intersections(
      const AABB<DerivedV1,3> & tree1,
      const Eigen::MatrixBase<DerivedV1> & V1,
      const Eigen::MatrixBase<DerivedF1> & F1,
      const Eigen::MatrixBase<DerivedV2> & V2,
      const Eigen::MatrixBase<DerivedF2> & F2,
      const bool first_only,
      Eigen::PlainObjectBase<DerivedIF> & IF,
      Eigen::PlainObjectBase<DerivedCP> & CP);
    /// \overload
    /// \brief Tree built internally.
    template <
      typename DerivedV1,
      typename DerivedF1,
      typename DerivedV2,
      typename DerivedF2,
      typename DerivedIF,
      typename DerivedCP>
    IGL_INLINE bool find_intersections(
      const Eigen::MatrixBase<DerivedV1> & V1,
      const Eigen::MatrixBase<DerivedF1> & F1,
      const Eigen::MatrixBase<DerivedV2> & V2,
      const Eigen::MatrixBase<DerivedF2> & F2,
      const bool first_only,
      Eigen::PlainObjectBase<DerivedIF> & IF,
      Eigen::PlainObjectBase<DerivedCP> & CP);
    /// @param[out] EV #EV by 3 list of vertex positions of intersection segments
    /// @param[out] EE #EE by 2 list of edge indices into EV
    /// @param[out] EI #EI by 1 list of indices into rows IF indicating source of
    ///   intersection.
    template <
      typename DerivedV1,
      typename DerivedF1,
      typename DerivedV2,
      typename DerivedF2,
      typename DerivedIF,
      typename DerivedCP,
      typename DerivedEV,
      typename DerivedEE,
      typename DerivedEI>
    IGL_INLINE bool find_intersections(
      const Eigen::MatrixBase<DerivedV1> & V1,
      const Eigen::MatrixBase<DerivedF1> & F1,
      const Eigen::MatrixBase<DerivedV2> & V2,
      const Eigen::MatrixBase<DerivedF2> & F2,
      Eigen::PlainObjectBase<DerivedIF> & IF,
      Eigen::PlainObjectBase<DerivedCP> & CP,
      Eigen::PlainObjectBase<DerivedEV> & EV,
      Eigen::PlainObjectBase<DerivedEE> & EE,
      Eigen::PlainObjectBase<DerivedEI> & EI);
  template <
    typename DerivedV1,
    typename DerivedF1,
    typename DerivedV2,
    typename DerivedF2,
    typename DerivedIF,
    typename DerivedCP,
    typename DerivedEV,
    typename DerivedEE,
    typename DerivedEI>
  IGL_INLINE bool find_intersections(
    const igl::AABB<DerivedV1,3> & tree1,
    const Eigen::MatrixBase<DerivedV1> & V1,
    const Eigen::MatrixBase<DerivedF1> & F1,
    const Eigen::MatrixBase<DerivedV2> & V2,
    const Eigen::MatrixBase<DerivedF2> & F2,
    Eigen::PlainObjectBase<DerivedIF> & IF,
    Eigen::PlainObjectBase<DerivedCP> & CP,
    Eigen::PlainObjectBase<DerivedEV> & EV,
    Eigen::PlainObjectBase<DerivedEE> & EE,
    Eigen::PlainObjectBase<DerivedEI> & EI);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "find_intersections.cpp"
#endif

#endif

