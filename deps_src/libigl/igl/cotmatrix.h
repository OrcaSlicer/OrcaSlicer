// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COTMATRIX_H
#define IGL_COTMATRIX_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

// History:
//  Used const references rather than copying the entire mesh 
//    Alec 9 October 2011
//  removed cotan (uniform weights) optional parameter it was building a buggy
//    half of the uniform laplacian, please see adjacency_matrix instead 
//    Alec 9 October 2011

namespace igl 
{
  // Constructs the cotangent stiffness matrix (discrete laplacian) for a given
  // mesh (V,F).
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
  // Outputs: 
  //   L  #V by #V cotangent matrix, each row i corresponding to V(i,:)
  //
  // See also: adjacency_matrix
  //
  // Note: This Laplacian uses the convention that diagonal entries are
  // **minus** the sum of off-diagonal entries. The diagonal entries are
  // therefore in general negative and the matrix is **negative** semi-definite
  // (immediately, -L is **positive** semi-definite)
  //
  template <typename DerivedV, typename DerivedF, typename Scalar>
  IGL_INLINE void cotmatrix(
    const Eigen::MatrixBase<DerivedV> & V, 
    const Eigen::MatrixBase<DerivedF> & F, 
    Eigen::SparseMatrix<Scalar>& L);
}

#ifndef IGL_STATIC_LIBRARY
#  include "cotmatrix.cpp"
#endif

#endif
