// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Gavin Barill <gavinpcb@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/

#ifndef IGL_OCTREE
#define IGL_OCTREE
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>




namespace igl
{
  // Given a set of 3D points P, generate data structures for a pointerless
  // octree. Each cell stores its points, children, center location and width.
  // Our octree is not dense. We use the following rule: if the current cell
  // has any number of points, it will have all 8 children. A leaf cell will
  // have -1's as its list of child indices.
  //
  // We use a binary numbering of children. Treating the parent cell's center
  // as the origin, we number the octants in the following manner:
  // The first bit is 1 iff the octant's x coordinate is positive
  // The second bit is 1 iff the octant's y coordinate is positive
  // The third bit is 1 iff the octant's z coordinate is positive
  //
  // For example, the octant with negative x, positive y, positive z is:
  // 110 binary = 6 decimal
  //
  // Inputs:
  //   P  #P by 3 list of point locations
  //
  // Outputs:
  //   point_indices  a vector of vectors, where the ith entry is a vector of
  //                  the indices into P that are the ith octree cell's points
  //   CH     #OctreeCells by 8, where the ith row is the indices of
  //          the ith octree cell's children
  //   CN     #OctreeCells by 3, where the ith row is a 3d row vector
  //          representing the position of the ith cell's center
  //   W      #OctreeCells, a vector where the ith entry is the width
  //          of the ith octree cell
  //
  template <typename DerivedP, typename IndexType, typename DerivedCH,
  typename DerivedCN, typename DerivedW>
  IGL_INLINE void octree(const Eigen::MatrixBase<DerivedP>& P,
    std::vector<std::vector<IndexType> > & point_indices,
    Eigen::PlainObjectBase<DerivedCH>& CH,
    Eigen::PlainObjectBase<DerivedCN>& CN,
    Eigen::PlainObjectBase<DerivedW>& W);
}

#ifndef IGL_STATIC_LIBRARY
#  include "octree.cpp"
#endif

#endif

