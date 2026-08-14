// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "edge_vectors.h"

#include <Eigen/Geometry>
#include "per_face_normals.h"

#include "PI.h"


template<typename DerivedV,typename DerivedF,typename DerivedE,
typename DerivedoE, typename Derivedvec>
IGL_INLINE void
igl::edge_vectors(
                  const Eigen::MatrixBase<DerivedV> &V,
                  const Eigen::MatrixBase<DerivedF> &F,
                  const Eigen::MatrixBase<DerivedE> &E,
                  const Eigen::MatrixBase<DerivedoE> &oE,
                  Eigen::PlainObjectBase<Derivedvec> &vec)
{
  Eigen::Matrix<typename Derivedvec::Scalar, Eigen::Dynamic, Eigen::Dynamic>
  dummy;
  edge_vectors<false>(V, F, E, oE, vec, dummy);
}


template<bool computePerpendicular,
typename DerivedV,typename DerivedF,typename DerivedE,
typename DerivedoE, typename DerivedvecParallel,
typename DerivedvecPerpendicular>
IGL_INLINE void
igl::edge_vectors(
                  const Eigen::MatrixBase<DerivedV> &V,
                  const Eigen::MatrixBase<DerivedF> &F,
                  const Eigen::MatrixBase<DerivedE> &E,
                  const Eigen::MatrixBase<DerivedoE> &oE,
                  Eigen::PlainObjectBase<DerivedvecParallel> &vecParallel,
                  Eigen::PlainObjectBase<DerivedvecPerpendicular> &vecPerpendicular)
{
  using Scalar = typename DerivedvecParallel::Scalar;
  using MatX = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
  
  assert(E.rows()==F.rows() && "E does not match dimensions of F.");
  assert(oE.rows()==F.rows() && "oE does not match dimensions of F.");
  assert(E.cols()==3 && F.cols()==3 && oE.cols()==3 &&
         "This method is for triangle meshes.");
  assert(F.maxCoeff()<V.rows() && "V does not seem to belong to F.");
  
  const typename DerivedE::Scalar m = E.maxCoeff()+1;
  
  //Compute edge-based normal
  MatX N, edgeN(m, 3);
  edgeN.setZero();
  per_face_normals(V, F, N);
  for(Eigen::Index i=0; i<E.rows(); ++i) {
    for(int j=0; j<3; ++j) {
      edgeN.row(E(i,j)) += N.row(i);
    }
  }
  edgeN.rowwise().normalize();
  
  //Compute edge vectors
  vecParallel.resize(m, 3);
  if(computePerpendicular) { //This should ideally be an if constexpr
    vecPerpendicular.resize(m, 3);
  }
  for(Eigen::Index i=0; i<E.rows(); ++i) {
    for(int j=0; j<3; ++j) {
      if(oE(i,j)<0) {
        continue;
      }
      const typename DerivedE::Scalar e=E(i,j);
      const typename DerivedF::Scalar vi=F(i,(j+1)%3), vj=F(i,(j+2)%3);
      vecParallel.row(e) = (V.row(vj)-V.row(vi)).normalized();
      if(computePerpendicular) { //This should ideally be an if constexpr
        vecPerpendicular.row(e) =
        Eigen::AngleAxis<Scalar>(0.5*PI, edgeN.row(e)) *
        vecParallel.row(e).transpose();
      }
    }
  }
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::edge_vectors<true, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
