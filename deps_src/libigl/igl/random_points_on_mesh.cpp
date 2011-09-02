// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "random_points_on_mesh.h"
#include "doublearea.h"
#include "cumsum.h"
#include "histc.h"
#include <iostream>
#include <cassert>

template <typename DerivedV, typename DerivedF, typename DerivedB, typename DerivedFI>
IGL_INLINE void igl::random_points_on_mesh(
  const int n,
  const Eigen::PlainObjectBase<DerivedV > & V,
  const Eigen::PlainObjectBase<DerivedF > & F,
  Eigen::PlainObjectBase<DerivedB > & B,
  Eigen::PlainObjectBase<DerivedFI > & FI)
{
  using namespace Eigen;
  using namespace std;
  typedef typename DerivedV::Scalar Scalar;
  typedef Matrix<Scalar,Dynamic,1> VectorXs;
  VectorXs A;
  doublearea(V,F,A);
  A /= A.array().sum();
  // Should be traingle mesh. Although Turk's method 1 generalizes...
  assert(F.cols() == 3);
  VectorXs C;
  VectorXs A0(A.size()+1);
  A0(0) = 0;
  A0.bottomRightCorner(A.size(),1) = A;
  // Even faster would be to use the "Alias Table Method"
  cumsum(A0,1,C);
  const VectorXs R = (VectorXs::Random(n,1).array() + 1.)/2.;
  assert(R.minCoeff() >= 0);
  assert(R.maxCoeff() <= 1);
  histc(R,C,FI);
  const VectorXs S = (VectorXs::Random(n,1).array() + 1.)/2.;
  const VectorXs T = (VectorXs::Random(n,1).array() + 1.)/2.;
  B.resize(n,3);
  B.col(0) = 1.-T.array().sqrt();
  B.col(1) = (1.-S.array()) * T.array().sqrt();
  B.col(2) = S.array() * T.array().sqrt();
}

template <typename DerivedV, typename DerivedF, typename ScalarB, typename DerivedFI>
IGL_INLINE void igl::random_points_on_mesh(
  const int n,
  const Eigen::PlainObjectBase<DerivedV > & V,
  const Eigen::PlainObjectBase<DerivedF > & F,
  Eigen::SparseMatrix<ScalarB > & B,
  Eigen::PlainObjectBase<DerivedFI > & FI)
{
  using namespace Eigen;
  using namespace std;
  Matrix<ScalarB,Dynamic,3> BC;
  random_points_on_mesh(n,V,F,BC,FI);
  vector<Triplet<ScalarB> > BIJV;
  BIJV.reserve(n*3);
  for(int s = 0;s<n;s++)
  {
    for(int c = 0;c<3;c++)
    {
      assert(FI(s) < F.rows());
      assert(FI(s) >= 0);
      const int v = F(FI(s),c);
      BIJV.push_back(Triplet<ScalarB>(s,v,BC(s,c)));
    }
  }
  B.resize(n,V.rows());
  B.reserve(n*3);
  B.setFromTriplets(BIJV.begin(),BIJV.end());
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::random_points_on_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::random_points_on_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
