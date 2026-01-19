// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BARYCENTRIC_COORDINATES_H
#define IGL_BARYCENTRIC_COORDINATES_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Compute barycentric coordinates in a tet
  //
  // Inputs:
  //   P  #P by 3 Query points in 3d
  //   A  #P by 3 Tet corners in 3d
  //   B  #P by 3 Tet corners in 3d
  //   C  #P by 3 Tet corners in 3d
  //   D  #P by 3 Tet corners in 3d
  // Outputs:
  //   L  #P by 4 list of barycentric coordinates
  //   
  template <
    typename DerivedP,
    typename DerivedA,
    typename DerivedB,
    typename DerivedC,
    typename DerivedD,
    typename DerivedL>
  IGL_INLINE void barycentric_coordinates(
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedA> & A,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<DerivedC> & C,
    const Eigen::MatrixBase<DerivedD> & D,
    Eigen::PlainObjectBase<DerivedL> & L);
  // Compute barycentric coordinates in a triangle
  //
  // Inputs:
  //   P  #P by dim Query points
  //   A  #P by dim Triangle corners
  //   B  #P by dim Triangle corners
  //   C  #P by dim Triangle corners
  // Outputs:
  //   L  #P by 3 list of barycentric coordinates
  //   
  template <
    typename DerivedP,
    typename DerivedA,
    typename DerivedB,
    typename DerivedC,
    typename DerivedL>
  IGL_INLINE void barycentric_coordinates(
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedA> & A,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedL> & L);

}

#ifndef IGL_STATIC_LIBRARY
#  include "barycentric_coordinates.cpp"
#endif

#endif
