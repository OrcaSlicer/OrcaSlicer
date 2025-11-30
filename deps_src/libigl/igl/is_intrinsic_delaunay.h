// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_INTRINSIC_DELAUNAY_H
#define IGL_IS_INTRINSIC_DELAUNAY_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  /// Determine if each edge in the mesh (V,F) is Delaunay.
  ///
  /// @param[in] l  #l by dim list of edge lengths
  /// @param[in] F  #F by 3 list of triangles indices
  /// @param[out] D  #F by 3 list of bools revealing whether edges corresponding 23 31 12
  ///     are locally Delaunay. Boundary edges are by definition Delaunay.
  ///     Non-Manifold edges are by definition not Delaunay.
  template <
    typename Derivedl,
    typename DerivedF,
    typename DerivedD>
  IGL_INLINE void is_intrinsic_delaunay(
    const Eigen::MatrixBase<Derivedl> & l,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedD> & D);
  /// \overload
  /// @param[in] uE2E  #uE list of lists mapping unique edges to (half-)edges
  template <
    typename Derivedl,
    typename DerivedF,
    typename uE2EType,
    typename DerivedD>
  IGL_INLINE void is_intrinsic_delaunay(
    const Eigen::MatrixBase<Derivedl> & l,
    const Eigen::MatrixBase<DerivedF> & F,
    const std::vector<std::vector<uE2EType> > & uE2E,
    Eigen::PlainObjectBase<DerivedD> & D);
  /// Determine whether a single edge is Delaunay using a provided (extrinsic) incirle
  /// test.
  ///
  /// @param[in] l  #l by dim list of edge lengths
  /// @param[in] uE2E  #uE list of lists of indices into E of coexisting edges (see
  ///              unique_edge_map)
  /// @param[in] num_faces  number of faces (==#F)
  /// @param[in] uei  index into uE2E of edge to check
  /// @return true iff edge is Delaunay
  template <
    typename Derivedl,
    typename uE2EType,
    typename Index>
  IGL_INLINE bool is_intrinsic_delaunay(
    const Eigen::MatrixBase<Derivedl> & l,
    const std::vector<std::vector<uE2EType> > & uE2E,
    const Index num_faces,
    const Index uei);

}
#ifndef IGL_STATIC_LIBRARY
#include "is_intrinsic_delaunay.cpp"
#endif
#endif

