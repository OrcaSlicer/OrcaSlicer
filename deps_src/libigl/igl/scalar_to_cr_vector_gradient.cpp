// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "scalar_to_cr_vector_gradient.h"

#include "orient_halfedges.h"

#include "doublearea.h"
#include "squared_edge_lengths.h"


template <typename DerivedV, typename DerivedF, typename DerivedE,
typename DerivedOE, typename ScalarG>
IGL_INLINE void
igl::scalar_to_cr_vector_gradient(
                                  const Eigen::MatrixBase<DerivedV>& V,
                                  const Eigen::MatrixBase<DerivedF>& F,
                                  const Eigen::MatrixBase<DerivedE>& E,
                                  const Eigen::MatrixBase<DerivedOE>& oE,
                                  Eigen::SparseMatrix<ScalarG>& G)
{
  Eigen::Matrix<typename DerivedV::Scalar, Eigen::Dynamic, Eigen::Dynamic>
  l_sq;
  squared_edge_lengths(V, F, l_sq);
  scalar_to_cr_vector_gradient_intrinsic(F, l_sq, E, oE, G);
}

template <typename DerivedV, typename DerivedF, typename DerivedE,
typename DerivedOE, typename ScalarG>
IGL_INLINE void
igl::scalar_to_cr_vector_gradient(
                                  const Eigen::MatrixBase<DerivedV>& V,
                                  const Eigen::MatrixBase<DerivedF>& F,
                                  Eigen::PlainObjectBase<DerivedE>& E,
                                  Eigen::PlainObjectBase<DerivedOE>& oE,
                                  Eigen::SparseMatrix<ScalarG>& G)
{
  if(E.rows()!=F.rows() || E.cols()!=F.cols() || oE.rows()!=F.rows() ||
     oE.cols()!=F.cols()) {
    orient_halfedges(F, E, oE);
  }
  
  const Eigen::PlainObjectBase<DerivedE>& cE = E;
  const Eigen::PlainObjectBase<DerivedOE>& coE = oE;

  scalar_to_cr_vector_gradient(V, F, cE, coE, G);
}


template <typename DerivedF, typename DerivedL_sq, typename DerivedE,
typename DerivedOE, typename ScalarG>
IGL_INLINE void
igl::scalar_to_cr_vector_gradient_intrinsic(
                                            const Eigen::MatrixBase<DerivedF>& F,
                                            const Eigen::MatrixBase<DerivedL_sq>& l_sq,
                                            const Eigen::MatrixBase<DerivedE>& E,
                                            const Eigen::MatrixBase<DerivedOE>& oE,
                                            Eigen::SparseMatrix<ScalarG>& G)
{
  Eigen::Matrix<typename DerivedL_sq::Scalar, Eigen::Dynamic, Eigen::Dynamic>
  dA;
  DerivedL_sq l_sqrt = l_sq.array().sqrt().matrix();
  doublearea(l_sqrt, dA);
  scalar_to_cr_vector_gradient_intrinsic(F, l_sq, dA, E, oE, G);
}


template <typename DerivedF, typename DerivedL_sq, typename DeriveddA,
typename DerivedE, typename DerivedOE, typename ScalarG>
IGL_INLINE void
igl::scalar_to_cr_vector_gradient_intrinsic(
                                            const Eigen::MatrixBase<DerivedF>& F,
                                            const Eigen::MatrixBase<DerivedL_sq>& l_sq,
                                            const Eigen::MatrixBase<DeriveddA>& dA,
                                            const Eigen::MatrixBase<DerivedE>& E,
                                            const Eigen::MatrixBase<DerivedOE>& oE,
                                            Eigen::SparseMatrix<ScalarG>& G)
{
  assert(F.cols()==3 && "Faces have three vertices");
  assert(E.rows()==F.rows() && E.cols()==F.cols() && oE.rows()==F.rows() &&
         oE.cols()==F.cols() && "Wrong dimension in edge vectors");
  
  const Eigen::Index m = F.rows();
  const typename DerivedF::Scalar n = F.maxCoeff() + 1;
  const typename DerivedE::Scalar nE = E.maxCoeff() + 1;
  
  std::vector<Eigen::Triplet<ScalarG> > tripletList;
  tripletList.reserve(5*3*m);
  for(Eigen::Index f=0; f<m; ++f) {
    for(int e=0; e<3; ++e) {
      const typename DerivedF::Scalar i=F(f,(e+1)%3), j=F(f,(e+2)%3), k=F(f,e);
      const ScalarG o=oE(f,e),
      eij=l_sq(f,e), ejk=l_sq(f,(e+1)%3), eki=l_sq(f,(e+2)%3); //These are squared quantities.
      const ScalarG s_eij = sqrt(eij);
      
      tripletList.emplace_back(E(f,e), i, -o*dA(f)/(6.*s_eij));
      tripletList.emplace_back(E(f,e)+nE, i, -o*(eij+ejk-eki)/(12.*s_eij));
      tripletList.emplace_back(E(f,e), j, o*dA(f)/(6.*s_eij));
      tripletList.emplace_back(E(f,e)+nE, j, -o*(eij-ejk+eki)/(12.*s_eij));
      tripletList.emplace_back(E(f,e)+nE, k, o*s_eij/6.);
    }
  }
  G.resize(2*nE, n);
  G.setFromTriplets(tripletList.begin(), tripletList.end());
}



#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::scalar_to_cr_vector_gradient_intrinsic<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int>&);
#endif
