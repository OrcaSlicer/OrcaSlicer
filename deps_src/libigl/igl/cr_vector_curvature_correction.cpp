// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "cr_vector_curvature_correction.h"

#include "orient_halfedges.h"
#include "gaussian_curvature.h"

#include "squared_edge_lengths.h"
#include "doublearea.h"
#include "boundary_loop.h"
#include "internal_angles_intrinsic.h"

#include "PI.h"


template <typename DerivedV, typename DerivedF, typename DerivedE,
typename DerivedOE, typename ScalarK>
IGL_INLINE void
igl::cr_vector_curvature_correction(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedE>& E,
  const Eigen::MatrixBase<DerivedOE>& oE,
  Eigen::SparseMatrix<ScalarK>& K)
{
  Eigen::Matrix<typename DerivedV::Scalar, Eigen::Dynamic, Eigen::Dynamic>
  l_sq;
  squared_edge_lengths(V, F, l_sq);
  cr_vector_curvature_correction_intrinsic(F, l_sq, E, oE, K);
}


template <typename DerivedV, typename DerivedF, typename DerivedE,
typename DerivedOE, typename ScalarK>
IGL_INLINE void
igl::cr_vector_curvature_correction(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedE>& E,
  Eigen::PlainObjectBase<DerivedOE>& oE,
  Eigen::SparseMatrix<ScalarK>& K)
{
  if(E.rows()!=F.rows() || E.cols()!=F.cols() || oE.rows()!=F.rows() ||
   oE.cols()!=F.cols()) {
    orient_halfedges(F, E, oE);
  }

  const Eigen::PlainObjectBase<DerivedE>& cE = E;
  const Eigen::PlainObjectBase<DerivedOE>& coE = oE;
  cr_vector_curvature_correction(V, F, cE, coE, K);
}


template <typename DerivedF, typename DerivedL_sq, typename DerivedE,
typename DerivedOE, typename ScalarK>
IGL_INLINE void
igl::cr_vector_curvature_correction_intrinsic(
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedL_sq>& l_sq,
  const Eigen::MatrixBase<DerivedE>& E,
  const Eigen::MatrixBase<DerivedOE>& oE,
  Eigen::SparseMatrix<ScalarK>& K)
{
  Eigen::Matrix<typename DerivedL_sq::Scalar,Eigen::Dynamic,Eigen::Dynamic>
  theta;
  internal_angles_intrinsic(l_sq, theta);
  
  cr_vector_curvature_correction_intrinsic(F, l_sq, theta, E, oE, K);
}


template <typename DerivedF, typename DerivedL_sq, typename Derivedtheta,
typename DerivedE, typename DerivedOE,
typename ScalarK>
IGL_INLINE void
igl::cr_vector_curvature_correction_intrinsic(
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedL_sq>& l_sq,
  const Eigen::MatrixBase<Derivedtheta>& theta,
  const Eigen::MatrixBase<DerivedE>& E,
  const Eigen::MatrixBase<DerivedOE>& oE,
  Eigen::SparseMatrix<ScalarK>& K)
{
  // Compute the angle defect kappa, set it to 0 at the boundary
  const typename DerivedF::Scalar n = F.maxCoeff() + 1;
  Eigen::Matrix<typename DerivedL_sq::Scalar,Eigen::Dynamic,1> kappa(n);
  kappa.setZero();
  for(Eigen::Index i=0; i<F.rows(); ++i) {
    for(int j=0; j<3; ++j) {
      kappa(F(i,j)) -= theta(i,j);
    }
  }
  kappa.array() += 2 * PI;
  std::vector<std::vector<typename DerivedF::Scalar> > b;
  boundary_loop(F, b);
  for(const auto& loop : b) {
    for(auto v : loop) {
      kappa(v) = 0;
    }
  }
    
  cr_vector_curvature_correction_intrinsic(F, l_sq, theta, kappa, E, oE, K);
}


template <typename DerivedF, typename DerivedL_sq, typename Derivedtheta,
typename Derivedkappa, typename DerivedE, typename DerivedOE,
typename ScalarK>
IGL_INLINE void
igl::cr_vector_curvature_correction_intrinsic(
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedL_sq>& l_sq,
  const Eigen::MatrixBase<Derivedtheta>& theta,
  const Eigen::MatrixBase<Derivedkappa>& kappa,
  const Eigen::MatrixBase<DerivedE>& E,
  const Eigen::MatrixBase<DerivedOE>& oE,
  Eigen::SparseMatrix<ScalarK>& K)
{
  assert(F.cols()==3 && "Faces have three vertices");
  assert(E.rows()==F.rows() && E.cols()==F.cols() && oE.rows()==F.rows() &&
   theta.rows()==F.rows() && theta.cols()==F.cols() &&
   oE.cols()==F.cols() && "Wrong dimension in edge vectors");
  assert(kappa.rows()==F.maxCoeff()+1 &&
   "Wrong dimension in theta or kappa");
  
  const Eigen::Index m = F.rows();
  const typename DerivedE::Scalar nE = E.maxCoeff() + 1;
  
  //Divide kappa by the actual angle sum to weigh consistently.
  Derivedtheta angleSum = Derivedtheta::Zero(kappa.rows(), 1);
  for(Eigen::Index i=0; i<F.rows(); ++i) {
    for(int j=0; j<3; ++j) {
      angleSum(F(i,j)) += theta(i,j);
    }
  }
  const Eigen::Matrix<typename Derivedkappa::Scalar, Eigen::Dynamic, 1>
  scaledKappa = kappa.array() / angleSum.array();
  
  std::vector<Eigen::Triplet<ScalarK> > tripletList;
  tripletList.reserve(10*3*m);
  for(Eigen::Index f=0; f<m; ++f) {
    for(int e=0; e<3; ++e) {
      const ScalarK eij=l_sq(f,e), ejk=l_sq(f,(e+1)%3),
      eki=l_sq(f,(e+2)%3); //These are squared quantities.
      const ScalarK lens = sqrt(eij*eki);
      const ScalarK o = oE(f,e)*oE(f,(e+2)%3);
      const typename DerivedF::Scalar i=F(f,(e+1)%3), j=F(f,(e+2)%3), k=F(f,e);
      const ScalarK ki=scaledKappa(i)*theta(f,(e+1)%3),
      kj=scaledKappa(j)*theta(f,(e+2)%3), kk=scaledKappa(k)*theta(f,e);
      
      const ScalarK costhetaidiv = (eij-ejk+eki)/(2.*lens);
      const ScalarK sinthetaidiv = sqrt( (1.-pow(eij-ejk+eki,2)/
        (4.*eij*eki)) );
      
      const ScalarK Corrijij = (ki+kj+kk);
      tripletList.emplace_back(E(f,e), E(f,e), Corrijij);
      tripletList.emplace_back(E(f,e)+nE, E(f,e)+nE, Corrijij);
      
      const ScalarK Corrijki = -o*(ki-kj-kk)*costhetaidiv;
      tripletList.emplace_back(E(f,e), E(f,(e+2)%3), Corrijki);
      tripletList.emplace_back(E(f,(e+2)%3), E(f,e), Corrijki);
      tripletList.emplace_back(E(f,e)+nE, E(f,(e+2)%3)+nE, Corrijki);
      tripletList.emplace_back(E(f,(e+2)%3)+nE, E(f,e)+nE, Corrijki);
      
      const ScalarK Corrijkiperp = o*(ki-kj-kk)*sinthetaidiv;
      tripletList.emplace_back(E(f,e), E(f,(e+2)%3)+nE, Corrijkiperp);
      tripletList.emplace_back(E(f,(e+2)%3)+nE, E(f,e), Corrijkiperp);
      tripletList.emplace_back(E(f,e)+nE, E(f,(e+2)%3), -Corrijkiperp);
      tripletList.emplace_back(E(f,(e+2)%3), E(f,e)+nE, -Corrijkiperp);
    }
  }
  
  K.resize(2*nE, 2*nE);
  K.setFromTriplets(tripletList.begin(), tripletList.end());
}



#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::cr_vector_curvature_correction_intrinsic<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int>&);
template void igl::cr_vector_curvature_correction<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::SparseMatrix<double, 0, int>&);
#endif
