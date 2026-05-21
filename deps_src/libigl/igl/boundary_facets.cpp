// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "boundary_facets.h"
#include "face_occurrences.h"
#include "list_to_matrix.h"
#include "matrix_to_list.h"
#include "sort.h"
#include "unique_rows.h"
#include "accumarray.h"

#include <Eigen/Core>

#include <map>
#include <iostream>

template <
  typename DerivedT, 
  typename DerivedF,
  typename DerivedJ,
  typename DerivedK>
IGL_INLINE void igl::boundary_facets(
  const Eigen::MatrixBase<DerivedT>& T,
  Eigen::PlainObjectBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedJ>& J,
  Eigen::PlainObjectBase<DerivedK>& K)
{
  const int simplex_size = T.cols();
  // Handle boring base case
  if(T.rows() == 0)
  {
    F.resize(0,simplex_size-1);
    J.resize(0,1);
    K.resize(0,1);
    return;
  }
  // Get a list of all facets/edges
  DerivedF allF(T.rows()*simplex_size,simplex_size-1);
  switch(simplex_size)
  {
    case 4:
      // Gather faces (e.g., loop over tets)
      for(int i = 0; i< (int)T.rows();i++)
      {
        // get face in correct order
        allF(i*simplex_size+0,2) = T(i,1);
        allF(i*simplex_size+0,1) = T(i,3);
        allF(i*simplex_size+0,0) = T(i,2);
        // get face in correct order
        allF(i*simplex_size+1,2) = T(i,0);
        allF(i*simplex_size+1,1) = T(i,2);
        allF(i*simplex_size+1,0) = T(i,3);
        // get face in correct order
        allF(i*simplex_size+2,2) = T(i,0);
        allF(i*simplex_size+2,1) = T(i,3);
        allF(i*simplex_size+2,0) = T(i,1);
        // get face in correct order
        allF(i*simplex_size+3,2) = T(i,0);
        allF(i*simplex_size+3,1) = T(i,1);
        allF(i*simplex_size+3,0) = T(i,2);
      }
      break;
    case 3:
      // Gather edges (loop over triangles)
      for(int i = 0; i< (int)T.rows();i++)
      {
        allF(i*simplex_size+0,0) = T(i,1);
        allF(i*simplex_size+0,1) = T(i,2);
        allF(i*simplex_size+1,0) = T(i,2);
        allF(i*simplex_size+1,1) = T(i,0);
        allF(i*simplex_size+2,0) = T(i,0);
        allF(i*simplex_size+2,1) = T(i,1);
      }
  }
  DerivedF sortedF;
  igl::sort(allF,2,true,sortedF);
  Eigen::VectorXi m,n;
  {
    DerivedF _1;
    igl::unique_rows(sortedF,_1,m,n);
  }
  Eigen::VectorXi C;
  igl::accumarray(n,1,C);
  const int ones = (C.array()==1).count();
  // Resize output to fit number of non-twos
  F.resize(ones, allF.cols());
  J.resize(F.rows(),1);
  K.resize(F.rows(),1);
  int k = 0;
  for(int c = 0;c< (int)C.size();c++)
  {
    if(C(c) == 1)
    {
      const int i = m(c);
      assert(k<(int)F.rows());
      F.row(k) = allF.row(i);
      J(k) = i/simplex_size;
      K(k) = i%simplex_size;
      k++;
    }
  }
  assert(k==(int)F.rows());
}

template <typename DerivedT, typename DerivedF>
IGL_INLINE void igl::boundary_facets(
  const Eigen::MatrixBase<DerivedT>& T,
  Eigen::PlainObjectBase<DerivedF>& F)
{
  Eigen::VectorXi J,K;
  return boundary_facets(T,F,J,K);
}

template <typename DerivedT, typename Ret>
Ret igl::boundary_facets(
  const Eigen::MatrixBase<DerivedT>& T)
{
  Ret F;
  igl::boundary_facets(T,F);
  return F;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::boundary_facets<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::boundary_facets<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >&);
template void igl::boundary_facets<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template Eigen::Matrix<int, -1, -1, 0, -1, -1> igl::boundary_facets<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
template void igl::boundary_facets<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
#endif
