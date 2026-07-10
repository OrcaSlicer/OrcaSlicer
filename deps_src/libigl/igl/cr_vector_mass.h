// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CR_VECTOR_MASS
#define IGL_CR_VECTOR_MASS

#include "igl_inline.h"

#include <Eigen/Core>
#include <Eigen/Sparse>


namespace igl
{
  /// Computes the CR vector mass matrix, using an arrangement of all parallel
  ///  degrees of freedom first, and all perpendicular degrees of freedom next.
  /// See Oded Stein, Max Wardetzky, Alec Jacobson, Eitan Grinspun, 2020.
  ///  "A Simple Discretization of the Vector Dirichlet Energy"
  ///
  ///  @param[in] V #V by 3 list of mesh vertex positions
  ///  @param[in] F #F by 3 list of mesh face indices into rows of V
  ///  @param[in] E #F by 3 a mapping from each halfedge to each edge
  ///  @param[out] L 2*|HE| by 2*|HE| computed Mass matrix
  template <typename DerivedV, typename DerivedF, typename DerivedE,
  typename ScalarM>
  IGL_INLINE void
  cr_vector_mass(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedE>& E,
    Eigen::SparseMatrix<ScalarM>& M);
  /// \overload
  ///
  /// \brief `E` are (possibly?) computed and output.
  template <typename DerivedV, typename DerivedF, typename DerivedE,
  typename ScalarM>
  IGL_INLINE void
  cr_vector_mass(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedE>& E,
    Eigen::SparseMatrix<ScalarM>& M);
  /// \overload
  /// \brief intrinsic version.
  ///
  ///  @param[in] dA #F list of double areas
  ///
  ///  \fileinfo
  template <typename DerivedF, typename DeriveddA,
  typename DerivedE, typename ScalarM>
  IGL_INLINE void
  cr_vector_mass_intrinsic(
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DeriveddA>& dA,
    const Eigen::MatrixBase<DerivedE>& E,
    Eigen::SparseMatrix<ScalarM>& M);

}


#ifndef IGL_STATIC_LIBRARY
#  include "cr_vector_mass.cpp"
#endif

#endif
