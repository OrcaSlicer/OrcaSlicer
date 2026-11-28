// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ADJACENCY_MATRIX_H
#define IGL_ADJACENCY_MATRIX_H
#include "igl_inline.h"

#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl 
{
  /// Constructs the graph adjacency matrix  of a given mesh (V,F)
  ///
  /// @tparam T  should be a eigen sparse matrix primitive type like `int` or `double`
  /// @param[in] F  #F by dim list of mesh simplices
  /// @param[out] A  max(F)+1 by max(F)+1 adjacency matrix, each row i corresponding to V(i,:)
  ///
  /// #### Example
  /// \code{.cpp}
  ///   // Mesh in (V,F)
  ///   Eigen::SparseMatrix<double> A;
  ///   adjacency_matrix(F,A);
  ///   // sum each row 
  ///   SparseVector<double> Asum;
  ///   sum(A,1,Asum);
  ///   // Convert row sums into diagonal of sparse matrix
  ///   SparseMatrix<double> Adiag;
  ///   diag(Asum,Adiag);
  ///   // Build uniform laplacian
  ///   SparseMatrix<double> U;
  ///   U = A-Adiag;
  /// \endcode
  ///
  /// \see 
  ///   edges,
  ///   cotmatrix,
  ///   diag
  template <typename DerivedF, typename T>
  IGL_INLINE void adjacency_matrix(
    const Eigen::MatrixBase<DerivedF> & F, 
    Eigen::SparseMatrix<T>& A);
  /// Constructs an vertex adjacency for a polygon mesh.
  ///
  /// @param[in] I  #I vectorized list of polygon corner indices into rows of some matrix V
  /// @param[in] C  #polygons+1 list of cumulative polygon sizes so that C(i+1)-C(i) =
  ///     size of the ith polygon, and so I(C(i)) through I(C(i+1)-1) are the
  ///     indices of the ith polygon
  /// @param[out] A  max(I)+1 by max(I)+1 adjacency matrix, each row i corresponding to V(i,:)
  ///
  template <typename DerivedI, typename DerivedC, typename T>
  IGL_INLINE void adjacency_matrix(
    const Eigen::MatrixBase<DerivedI> & I,
    const Eigen::MatrixBase<DerivedC> & C,
    Eigen::SparseMatrix<T>& A);
}

#ifndef IGL_STATIC_LIBRARY
#  include "adjacency_matrix.cpp"
#endif

#endif
