// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "project_isometrically_to_plane.h"
#include "edge_lengths.h"

template <
  typename DerivedV, 
  typename DerivedF,
  typename DerivedU,
  typename DerivedUF,
  typename Scalar>
IGL_INLINE void igl::project_isometrically_to_plane(
  const Eigen::PlainObjectBase<DerivedV> & V, 
  const Eigen::PlainObjectBase<DerivedF> & F, 
  Eigen::PlainObjectBase<DerivedU> & U,
  Eigen::PlainObjectBase<DerivedUF> & UF, 
  Eigen::SparseMatrix<Scalar>& I)
{
  using namespace std;
  using namespace Eigen;
  assert(F.cols() == 3 && "F should contain triangles");
  typedef DerivedV MatrixX;
  MatrixX l;
  edge_lengths(V,F,l);
  // Number of faces
  const int m = F.rows();

  // First corner at origin
  U = DerivedU::Zero(m*3,2);
  // Second corner along x-axis
  U.block(m,0,m,1) = l.col(2);
  // Third corner rotated onto plane
  U.block(m*2,0,m,1) = 
    (-l.col(0).array().square() + 
     l.col(1).array().square() + 
     l.col(2).array().square())/(2.*l.col(2).array());
  U.block(m*2,1,m,1) =
    (l.col(1).array().square()-U.block(m*2,0,m,1).array().square()).sqrt();

  typedef Triplet<Scalar> IJV;
  vector<IJV > ijv;
  ijv.reserve(3*m);
  UF.resize(m,3);
  for(int f = 0;f<m;f++)
  {
    for(int c = 0;c<3;c++)
    {
      UF(f,c) = c*m+f;
      ijv.push_back(IJV(F(f,c),c*m+f,1));
    }
  }
  I.resize(V.rows(),m*3);
  I.setFromTriplets(ijv.begin(),ijv.end());
}

#ifdef IGL_STATIC_LIBRARY
template void igl::project_isometrically_to_plane<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::SparseMatrix<double, 0, int>&);
#endif
