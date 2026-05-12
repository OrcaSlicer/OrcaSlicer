// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "orientable_patches.h"
#include "vertex_components.h"
#include "sort.h"
#include "unique_rows.h"
#include <vector>
#include <iostream>

template <typename DerivedF, typename DerivedC, typename AScalar>
IGL_INLINE void igl::orientable_patches(
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedC> & C,
  Eigen::SparseMatrix<AScalar> & A)
{
  using namespace Eigen;
  using namespace std;

  // simplex size
  assert(F.cols() == 3);

  // List of all "half"-edges: 3*#F by 2
  Matrix<typename DerivedF::Scalar, Dynamic, 2> allE,sortallE,uE;
  allE.resize(F.rows()*3,2);
  Matrix<typename DerivedF::Scalar,Dynamic,2> IX;
  VectorXi IA,IC;
  allE.block(0*F.rows(),0,F.rows(),1) = F.col(1);
  allE.block(0*F.rows(),1,F.rows(),1) = F.col(2);
  allE.block(1*F.rows(),0,F.rows(),1) = F.col(2);
  allE.block(1*F.rows(),1,F.rows(),1) = F.col(0);
  allE.block(2*F.rows(),0,F.rows(),1) = F.col(0);
  allE.block(2*F.rows(),1,F.rows(),1) = F.col(1);
  // Sort each row
  sort(allE,2,true,sortallE,IX);
  //IC(i) tells us where to find sortallE(i,:) in uE: 
  // so that sortallE(i,:) = uE(IC(i),:)
  unique_rows(sortallE,uE,IA,IC);
  // uE2FT(e,f) = 1 means face f is adjacent to unique edge e
  vector<Triplet<AScalar> > uE2FTijv(IC.rows());
  for(int e = 0;e<IC.rows();e++)
  {
    uE2FTijv[e] = Triplet<AScalar>(e%F.rows(),IC(e),1);
  }
  SparseMatrix<AScalar> uE2FT(F.rows(),uE.rows());
  uE2FT.setFromTriplets(uE2FTijv.begin(),uE2FTijv.end());
  // kill non-manifold edges
  for(int j=0; j<(int)uE2FT.outerSize();j++)
  {
    int degree = 0;
    for(typename SparseMatrix<AScalar>::InnerIterator it (uE2FT,j); it; ++it)
    {
      degree++;
    }
    // Iterate over inside
    if(degree > 2)
    {
      for(typename SparseMatrix<AScalar>::InnerIterator it (uE2FT,j); it; ++it)
      {
        uE2FT.coeffRef(it.row(),it.col()) = 0;
      }
    }
  }
  // Face-face Adjacency matrix
  SparseMatrix<AScalar> uE2F;
  uE2F = uE2FT.transpose().eval();
  A = uE2FT*uE2F;
  // All ones
  for(int j=0; j<A.outerSize();j++)
  {
    // Iterate over inside
    for(typename SparseMatrix<AScalar>::InnerIterator it (A,j); it; ++it)
    {
      if(it.value() > 1)
      {
        A.coeffRef(it.row(),it.col()) = 1;
      }
    }
  }
  //% Connected components are patches
  //%C = vertex_components(A); % alternative to graphconncomp from matlab_bgl
  //[~,C] = graphconncomp(A);
  // graph connected components 
  vertex_components(A,C);

}

template <typename DerivedF, typename DerivedC>
IGL_INLINE void igl::orientable_patches(
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedC> & C)
{
  Eigen::SparseMatrix<typename DerivedF::Scalar> A;
  return orientable_patches(F,C,A);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::orientable_patches<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, int>(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::SparseMatrix<int, 0, int>&);
template void igl::orientable_patches<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::orientable_patches<Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, int>(Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::SparseMatrix<int, 0, int>&);
template void igl::orientable_patches<Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
