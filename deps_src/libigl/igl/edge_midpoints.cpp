// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "edge_midpoints.h"

template<typename DerivedV,typename DerivedF,typename DerivedE,
typename DerivedoE, typename Derivedmps>
IGL_INLINE void
igl::edge_midpoints(
	const Eigen::MatrixBase<DerivedV> &V,
	const Eigen::MatrixBase<DerivedF> &F,
	const Eigen::MatrixBase<DerivedE> &E,
	const Eigen::MatrixBase<DerivedoE> &oE,
	Eigen::PlainObjectBase<Derivedmps> &mps)
{
  assert(E.rows()==F.rows() && "E does not match dimensions of F.");
  assert(oE.rows()==F.rows() && "oE does not match dimensions of F.");
  assert(E.cols()==3 && F.cols()==3 && oE.cols()==3 &&
    "This method is for triangle meshes.");
  assert(F.maxCoeff()<V.rows() && "V does not seem to belong to F.");

  using ScalarE = typename DerivedE::Scalar;
  using ScalarF = typename DerivedF::Scalar;
	
  const ScalarE m = E.maxCoeff()+1;
	
  mps.resize(m, V.cols());
  for(Eigen::Index i=0; i<F.rows(); ++i) {
    for(int j=0; j<3; ++j) {
      if(oE(i,j)<0) {
        continue;
      }
      const ScalarE e = E(i,j);
      const ScalarF vi=F(i,(j+1)%3), vj=F(i,(j+2)%3);

      mps.row(e) = 0.5*(V.row(vi) + V.row(vj));
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::edge_midpoints<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
