// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_REMESH_ALONG_ISOLINE_H
#define IGL_REMESH_ALONG_ISOLINE_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl 
{
  // Given a triangle mesh and a scalar field, remesh so that a given isovalue
  // of the scalar field follows (new) edges of the output mesh. Effectively
  // running "marching triangles" on mesh, but not in any coherent order. The
  // output mesh should be as manifold as the input.
  //
  // Inputs:
  //   V  #V by dim list of mesh vertex positions
  //   F  #F by 3 list of mesh triangle indices into V
  //   S  #V by 1 list of scalar field
  //   val  value of S to remesh along
  // Outputs:
  //  U  #U by dim list of mesh vertex positions #U>=#V
  //  G  #G by 3 list of mesh triangle indices into U, #G>=#F
  //  SU  #U list of scalar field values over new mesh
  //  J  #G list of indices into G revealing birth triangles
  //  BC  #U by #V sparse matrix of barycentric coordinates so that U = BC*V
  //  L  #G list of bools whether scalar field in triangle below or above val
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedS,
    typename DerivedU,
    typename DerivedG,
    typename DerivedJ,
    typename BCtype,
    typename DerivedSU,
    typename DerivedL>
    IGL_INLINE void remesh_along_isoline(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      const Eigen::MatrixBase<DerivedS> & S,
      const typename DerivedS::Scalar val,
      Eigen::PlainObjectBase<DerivedU> & U,
      Eigen::PlainObjectBase<DerivedG> & G,
      Eigen::PlainObjectBase<DerivedSU> & SU,
      Eigen::PlainObjectBase<DerivedJ> & J,
      Eigen::SparseMatrix<BCtype> & BC,
      Eigen::PlainObjectBase<DerivedL> & L);
  // Input:
  //   n  number of vertices (#V)
  template <
    typename DerivedF,
    typename DerivedS,
    typename DerivedG,
    typename DerivedJ,
    typename BCtype,
    typename DerivedSU,
    typename DerivedL>
    IGL_INLINE void remesh_along_isoline(
      const int n,
      const Eigen::MatrixBase<DerivedF> & F,
      const Eigen::MatrixBase<DerivedS> & S,
      const typename DerivedS::Scalar val,
      Eigen::PlainObjectBase<DerivedG> & G,
      Eigen::PlainObjectBase<DerivedSU> & SU,
      Eigen::PlainObjectBase<DerivedJ> & J,
      Eigen::SparseMatrix<BCtype> & BC,
      Eigen::PlainObjectBase<DerivedL> & L);
}

#ifndef IGL_STATIC_LIBRARY
#  include "remesh_along_isoline.h"
#endif

#endif
