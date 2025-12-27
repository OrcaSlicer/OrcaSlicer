// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CROUZEIX_RAVIART_COTMATRIX
#define IGL_CROUZEIX_RAVIART_COTMATRIX
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  // CROUZEIX_RAVIART_COTMATRIX Compute the Crouzeix-Raviart cotangent
  // stiffness matrix.
  //
  // See for example "Discrete Quadratic Curvature Energies" [Wardetzky, Bergou,
  // Harmon, Zorin, Grinspun 2007]
  //
  // Inputs:
  //   V  #V by dim list of vertex positions
  //   F  #F by 3/4 list of triangle/tetrahedron indices
  // Outputs:
  //   L  #E by #E edge/face-based diagonal cotangent matrix
  //   E  #E by 2/3 list of edges/faces
  //   EMAP  #F*3/4 list of indices mapping allE to E
  //
  // See also: crouzeix_raviart_massmatrix
  template <typename DerivedV, typename DerivedF, typename LT, typename DerivedE, typename DerivedEMAP>
  void crouzeix_raviart_cotmatrix(
      const Eigen::MatrixBase<DerivedV> & V, 
      const Eigen::MatrixBase<DerivedF> & F, 
      Eigen::SparseMatrix<LT> & L,
      Eigen::PlainObjectBase<DerivedE> & E,
      Eigen::PlainObjectBase<DerivedEMAP> & EMAP);
  // wrapper if E and EMAP are already computed (better match!)
  template <typename DerivedV, typename DerivedF, typename DerivedE, typename DerivedEMAP, typename LT>
  void crouzeix_raviart_cotmatrix(
      const Eigen::MatrixBase<DerivedV> & V, 
      const Eigen::MatrixBase<DerivedF> & F, 
      const Eigen::MatrixBase<DerivedE> & E,
      const Eigen::MatrixBase<DerivedEMAP> & EMAP,
      Eigen::SparseMatrix<LT> & L);
}
#ifndef IGL_STATIC_LIBRARY
#  include "crouzeix_raviart_cotmatrix.cpp"
#endif
#endif
