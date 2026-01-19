// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IN_ELEMENT_H
#define IGL_IN_ELEMENT_H

#include "igl_inline.h"
#include "AABB.h"
#include <Eigen/Core>
#include <Eigen/Sparse>

namespace igl
{
  // Determine whether each point in a list of points is in the elements of a
  // mesh.
  //
  // templates:
  //   DIM  dimension of vertices in V (# of columns)
  // Inputs:
  //   V  #V by dim list of mesh vertex positions.
  //   Ele  #Ele by dim+1 list of mesh indices into #V.
  //   Q  #Q by dim list of query point positions
  //   aabb  axis-aligned bounding box tree object (see AABB.h)
  // Outputs:
  //   I  #Q list of indices into Ele of first containing element (-1 means no
  //     containing element)
  template <typename DerivedV, typename DerivedQ, int DIM>
  IGL_INLINE void in_element(
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::MatrixXi & Ele,
    const Eigen::PlainObjectBase<DerivedQ> & Q,
    const AABB<DerivedV,DIM> & aabb,
    Eigen::VectorXi & I);
  // Outputs:
  //   I  #Q by #Ele sparse matrix revealing whether each element contains each
  //     point: I(q,e) means point q is in element e
  template <typename DerivedV, typename DerivedQ, int DIM, typename Scalar>
  IGL_INLINE void in_element(
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::MatrixXi & Ele,
    const Eigen::PlainObjectBase<DerivedQ> & Q,
    const AABB<DerivedV,DIM> & aabb,
    Eigen::SparseMatrix<Scalar> & I);
};

#ifndef IGL_STATIC_LIBRARY
#include "in_element.cpp"
#endif

#endif
