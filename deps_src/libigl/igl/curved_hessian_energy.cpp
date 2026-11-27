// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "curved_hessian_energy.h"

#include "orient_halfedges.h"
#include "doublearea.h"
#include "squared_edge_lengths.h"
#include "cr_vector_laplacian.h"
#include "cr_vector_mass.h"
#include "cr_vector_curvature_correction.h"
#include "scalar_to_cr_vector_gradient.h"


template <typename DerivedV, typename DerivedF, typename ScalarQ>
IGL_INLINE void
igl::curved_hessian_energy(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::SparseMatrix<ScalarQ>& Q)
{
  Eigen::MatrixXi E, oE;
  curved_hessian_energy(V, F, E, oE, Q);
}


template <typename DerivedV, typename DerivedF, typename DerivedE,
typename DerivedOE, typename ScalarQ>
IGL_INLINE void
igl::curved_hessian_energy(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedE>& E,
  const Eigen::MatrixBase<DerivedOE>& oE,
  Eigen::SparseMatrix<ScalarQ>& Q)
{
  Eigen::Matrix<typename DerivedV::Scalar, Eigen::Dynamic, Eigen::Dynamic>
  l_sq;
  squared_edge_lengths(V, F, l_sq);
  curved_hessian_energy_intrinsic(F, l_sq, E, oE, Q);
}


template <typename DerivedV, typename DerivedF, typename DerivedE,
typename DerivedOE, typename ScalarQ>
IGL_INLINE void
igl::curved_hessian_energy(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedE>& E,
  Eigen::PlainObjectBase<DerivedOE>& oE,
  Eigen::SparseMatrix<ScalarQ>& Q)
{
  if(E.rows()!=F.rows() || E.cols()!=F.cols() || oE.rows()!=F.rows() ||
   oE.cols()!=F.cols()) {
    orient_halfedges(F, E, oE);
  }

  const Eigen::PlainObjectBase<DerivedE>& cE = E;
  const Eigen::PlainObjectBase<DerivedOE>& coE = oE;
  curved_hessian_energy(V, F, cE, coE, Q);
}


template <typename DerivedF, typename DerivedL_sq, typename DerivedE,
typename DerivedOE, typename ScalarQ>
IGL_INLINE void
igl::curved_hessian_energy_intrinsic(
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedL_sq>& l_sq,
  const Eigen::MatrixBase<DerivedE>& E,
  const Eigen::MatrixBase<DerivedOE>& oE,
  Eigen::SparseMatrix<ScalarQ>& Q)
{
  Eigen::Matrix<typename DerivedL_sq::Scalar, Eigen::Dynamic, Eigen::Dynamic>
  dA;
  Eigen::Matrix<typename DerivedL_sq::Scalar, Eigen::Dynamic, Eigen::Dynamic>
  l_sqrt = l_sq.array().sqrt().matrix();
  doublearea(l_sqrt, dA);
  curved_hessian_energy_intrinsic(F, l_sq, dA, E, oE, Q);
}


template <typename DerivedF, typename DerivedL_sq, typename DeriveddA,
typename DerivedE, typename DerivedOE, typename ScalarQ>
IGL_INLINE void
igl::curved_hessian_energy_intrinsic(
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedL_sq>& l_sq,
  const Eigen::MatrixBase<DeriveddA>& dA,
  const Eigen::MatrixBase<DerivedE>& E,
  const Eigen::MatrixBase<DerivedOE>& oE,
  Eigen::SparseMatrix<ScalarQ>& Q)
{
  //Matrices that need to be combined
  Eigen::SparseMatrix<ScalarQ> M, D, L, K;
  cr_vector_mass_intrinsic(F, dA,  E, M);
  scalar_to_cr_vector_gradient_intrinsic(F, l_sq, dA, E, oE, D);
  cr_vector_laplacian_intrinsic(F, l_sq, dA, E, oE, L);
  cr_vector_curvature_correction_intrinsic(F, l_sq, E, oE, K);

  //Invert M
  std::vector<Eigen::Triplet<ScalarQ> > tripletListMi;
  for(Eigen::Index k=0; k<M.outerSize(); ++k) {
    for(typename Eigen::SparseMatrix<ScalarQ>::InnerIterator it(M,k);
      it; ++it) {
      if(it.value() > 0) {
        tripletListMi.emplace_back(it.row(), it.col(), 1./it.value());
      }
    }
  }
  Eigen::SparseMatrix<ScalarQ> Mi(M.rows(), M.cols());
  Mi.setFromTriplets(tripletListMi.begin(), tripletListMi.end());

  //Hessian energy matrix
  Q = D.transpose()*Mi*(L + K)*Mi*D;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::curved_hessian_energy<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int>&);
#endif
