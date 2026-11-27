// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CR_VECTOR_CURVATURE_CORRECTION_H
#define IGL_CR_VECTOR_CURVATURE_CORRECTION_H

#include "igl_inline.h"

#include <Eigen/Core>
#include <Eigen/Sparse>


namespace igl
{
  /// Computes the vector Crouzeix-Raviart curvature correction
  ///  term of Oded Stein, Alec Jacobson, Max Wardetzky, Eitan
  ///  Grinspun, 2020. "A Smoothness Energy without Boundary Distortion for
  ///  Curved Surfaces", but using the basis functions by Oded Stein,
  ///  Max Wardetzky, Alec Jacobson, Eitan Grinspun, 2020.
  ///  "A Simple Discretization of the Vector Dirichlet Energy"
  ///
  ///  @param[in] V #V by 3 list of mesh vertex positions
  ///  @param[in] F #F by 3 list of mesh face indices into rows of V
  ///  @param[in] E #F by 4 a mapping from each halfedge to each edge
  ///  @param[in] oE #F by 3 the orientation (e.g., -1 or 1) of each halfedge
  ///    compared to the orientation of the actual edge, as computed with
  ///    orient_halfedges. will be computed if not provided.
  ///  @param[out] K 2*|HE| by 2*|HE| computed curvature correction matrix
  template <typename DerivedV, typename DerivedF, typename DerivedE,
  typename DerivedOE, typename ScalarK>
  IGL_INLINE void
  cr_vector_curvature_correction(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedE>& E,
    const Eigen::MatrixBase<DerivedOE>& oE,
    Eigen::SparseMatrix<ScalarK>& K);
  /// \overload
  ///
  /// \brief `E` and `oE` are computed and output.
  template <typename DerivedV, typename DerivedF, typename DerivedE,
  typename DerivedOE, typename ScalarK>
  IGL_INLINE void
  cr_vector_curvature_correction(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedE>& E,
    Eigen::PlainObjectBase<DerivedOE>& oE,
    Eigen::SparseMatrix<ScalarK>& K);
  /// \overload
  /// \brief intrinsic version.
  ///
  ///  @param[in] l_sq #F by 3 list of squared edge lengths of each halfedge
  ///  @param[in] theta #F by 3 list of the tip angles at each halfedge
  ///  @param[in] kappa #V by 1 list of the Gaussian curvature at each vertex
  ///
  ///  \fileinfo
  template <typename DerivedF, typename DerivedL_sq, typename Derivedtheta,
  typename Derivedkappa, typename DerivedE, typename DerivedOE,
  typename ScalarK>
  IGL_INLINE void
  cr_vector_curvature_correction_intrinsic(
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedL_sq>& l_sq,
    const Eigen::MatrixBase<Derivedtheta>& theta,
    const Eigen::MatrixBase<Derivedkappa>& kappa,
    const Eigen::MatrixBase<DerivedE>& E,
    const Eigen::MatrixBase<DerivedOE>& oE,
    Eigen::SparseMatrix<ScalarK>& K);
  ///  \overload
  ///  \fileinfo
  template <typename DerivedF, typename DerivedL_sq, typename DerivedE,
  typename DerivedOE, typename ScalarK>
  IGL_INLINE void
  cr_vector_curvature_correction_intrinsic(
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedL_sq>& l_sq,
    const Eigen::MatrixBase<DerivedE>& E,
    const Eigen::MatrixBase<DerivedOE>& oE,
    Eigen::SparseMatrix<ScalarK>& K);
  ///  \overload
  ///  \fileinfo
  template <typename DerivedF, typename DerivedL_sq, typename Derivedtheta,
  typename DerivedE, typename DerivedOE,
  typename ScalarK>
  IGL_INLINE void
  cr_vector_curvature_correction_intrinsic(
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedL_sq>& l_sq,
    const Eigen::MatrixBase<Derivedtheta>& theta,
    const Eigen::MatrixBase<DerivedE>& E,
    const Eigen::MatrixBase<DerivedOE>& oE,
    Eigen::SparseMatrix<ScalarK>& K);
}


#ifndef IGL_STATIC_LIBRARY
#  include "cr_vector_curvature_correction.cpp"
#endif

#endif
