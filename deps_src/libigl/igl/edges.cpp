// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "edges.h"
#include "adjacency_matrix.h"
#include <iostream>

template <typename DerivedF, typename DerivedE>
IGL_INLINE void igl::edges(
  const Eigen::MatrixBase<DerivedF> & F, 
  Eigen::PlainObjectBase<DerivedE> & E)
{
  // build adjacency matrix
  typedef typename DerivedF::Scalar Index;
  Eigen::SparseMatrix<Index> A;
  igl::adjacency_matrix(F,A);
  // Number of non zeros should be twice number of edges
  assert(A.nonZeros()%2 == 0);
  // Resize to fit edges
  E.resize(A.nonZeros()/2,2);
  int i = 0;
  // Iterate over outside
  for(int k=0; k<A.outerSize(); ++k)
  {
    // Iterate over inside
    for(typename Eigen::SparseMatrix<Index>::InnerIterator it (A,k); it; ++it)
    {
      // only add edge in one direction
      if(it.row()<it.col())
      {
        E(i,0) = it.row();
        E(i,1) = it.col();
        i++;
      }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::edges<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 2, 0, -1, 2> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> >&);
template void igl::edges<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
