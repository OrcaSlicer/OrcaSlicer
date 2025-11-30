// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Gavin Barill <gavinpcb@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/

#ifndef IGL_KNN_H
#define IGL_KNN_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  /// Given a 3D set of points P, an whole number k, and an octree
  /// find the indicies of the k nearest neighbors for each point in P.
  /// Note that each point is its own neighbor.
  ///
  /// The octree data structures used in this function are intended to be the
  /// same ones output from igl::octree
  ///
  /// @param[in] P  #P by 3 list of point locations
  /// @param[in] k  number of neighbors to find
  /// @param[in] point_indices  a vector of vectors, where the ith entry is a vector of
  ///                           the indices into P that are the ith octree cell's points
  /// @param[in] CH     #OctreeCells by 8, where the ith row is the indices of
  ///                   the ith octree cell's children
  /// @param[in] CN     #OctreeCells by 3, where the ith row is a 3d row vector
  ///                   representing the position of the ith cell's center
  /// @param[in] W      #OctreeCells, a vector where the ith entry is the width
  ///          of the ith octree cell
  /// @param[out] I  #P by k list of k-nearest-neighbor indices into P
  template <
    typename DerivedP, 
    typename IndexType,
    typename DerivedCH,
    typename DerivedCN,
    typename DerivedW,
    typename DerivedI>
  IGL_INLINE void knn(
    const Eigen::MatrixBase<DerivedP>& P,
    size_t k,
    const std::vector<std::vector<IndexType> > & point_indices,
    const Eigen::MatrixBase<DerivedCH>& CH,
    const Eigen::MatrixBase<DerivedCN>& CN,
    const Eigen::MatrixBase<DerivedW>& W,
    Eigen::PlainObjectBase<DerivedI> & I);
  /// \overload
  /// \brief only neighbors found in V
  /// @param[in] V  #V by 3 list of point locations for which may be neighbors 
  /// @param[out] I  #P by k list of k-nearest-neighbor indices into V
  template <
    typename DerivedP, 
    typename DerivedV,
    typename IndexType,
    typename DerivedCH,
    typename DerivedCN,
    typename DerivedW,
    typename DerivedI>
  IGL_INLINE void knn(
    const Eigen::MatrixBase<DerivedP>& P,
    const Eigen::MatrixBase<DerivedV>& V,
    size_t k,
    const std::vector<std::vector<IndexType> > & point_indices,
    const Eigen::MatrixBase<DerivedCH>& CH,
    const Eigen::MatrixBase<DerivedCN>& CN,
    const Eigen::MatrixBase<DerivedW>& W,
    Eigen::PlainObjectBase<DerivedI> & I);
}
#ifndef IGL_STATIC_LIBRARY
#  include "knn.cpp"
#endif
#endif

