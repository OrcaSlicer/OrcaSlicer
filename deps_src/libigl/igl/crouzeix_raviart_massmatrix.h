// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef CROUZEIX_RAVIART_MASSMATRIX_H
#define CROUZEIX_RAVIART_MASSMATRIX_H
#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl
{
  // CROUZEIX_RAVIART_MASSMATRIX Compute the Crouzeix-Raviart mass matrix where
  // M(e,e) is just the sum of the areas of the triangles on either side of an
  // edge e.
  //
  // See for example "Discrete Quadratic Curvature Energies" [Wardetzky, Bergou,
  // Harmon, Zorin, Grinspun 2007]
  //
  // Inputs:
  //   V  #V by dim list of vertex positions
  //   F  #F by 3/4 list of triangle/tetrahedron indices
  // Outputs:
  //   M  #E by #E edge/face-based diagonal mass matrix
  //   E  #E by 2/3 list of edges/faces
  //   EMAP  #F*3/4 list of indices mapping allE to E
  //
  // See also: crouzeix_raviart_cotmatrix
  template <typename MT, typename DerivedV, typename DerivedF, typename DerivedE, typename DerivedEMAP>
  void crouzeix_raviart_massmatrix(
      const Eigen::MatrixBase<DerivedV> & V, 
      const Eigen::MatrixBase<DerivedF> & F, 
      Eigen::SparseMatrix<MT> & M,
      Eigen::PlainObjectBase<DerivedE> & E,
      Eigen::PlainObjectBase<DerivedEMAP> & EMAP);
  // wrapper if E and EMAP are already computed (better match!)
  template <typename MT, typename DerivedV, typename DerivedF, typename DerivedE, typename DerivedEMAP>
  void crouzeix_raviart_massmatrix(
      const Eigen::MatrixBase<DerivedV> & V, 
      const Eigen::MatrixBase<DerivedF> & F, 
      const Eigen::MatrixBase<DerivedE> & E,
      const Eigen::MatrixBase<DerivedEMAP> & EMAP,
      Eigen::SparseMatrix<MT> & M);
}
#ifndef IGL_STATIC_LIBRARY
#  include "crouzeix_raviart_massmatrix.cpp"
#endif
  
#endif
