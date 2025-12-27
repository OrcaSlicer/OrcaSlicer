// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MASSMATRIX_TYPE_H
#define IGL_MASSMATRIX_TYPE_H
#include "igl_inline.h"

#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl 
{

  enum MassMatrixType
  {
    MASSMATRIX_TYPE_BARYCENTRIC = 0,
    MASSMATRIX_TYPE_VORONOI = 1,
    MASSMATRIX_TYPE_FULL = 2,
    MASSMATRIX_TYPE_DEFAULT = 3,
    NUM_MASSMATRIX_TYPE = 4
  };

  // Constructs the mass (area) matrix for a given mesh (V,F).
  //
  // Templates:
  //   DerivedV  derived type of eigen matrix for V (e.g. derived from
  //     MatrixXd)
  //   DerivedF  derived type of eigen matrix for F (e.g. derived from
  //     MatrixXi)
  //   Scalar  scalar type for eigen sparse matrix (e.g. double)
  // Inputs:
  //   V  #V by dim list of mesh vertex positions
  //   F  #F by simplex_size list of mesh faces (must be triangles)
  //   type  one of the following ints:
  //     MASSMATRIX_TYPE_BARYCENTRIC  barycentric
  //     MASSMATRIX_TYPE_VORONOI voronoi-hybrid {default}
  //     MASSMATRIX_TYPE_FULL full {not implemented}
  // Outputs: 
  //   M  #V by #V mass matrix
  //
  // See also: adjacency_matrix
  //
  template <typename DerivedV, typename DerivedF, typename Scalar>
  IGL_INLINE void massmatrix(
    const Eigen::MatrixBase<DerivedV> & V, 
    const Eigen::MatrixBase<DerivedF> & F, 
    const MassMatrixType type,
    Eigen::SparseMatrix<Scalar>& M);
}

#ifndef IGL_STATIC_LIBRARY
#  include "massmatrix.cpp"
#endif

#endif

