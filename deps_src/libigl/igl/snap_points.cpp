// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "snap_points.h"
#include <cassert>
#include <limits>

template <
  typename DerivedC, 
  typename DerivedV, 
  typename DerivedI, 
  typename DerivedminD, 
  typename DerivedVI>
IGL_INLINE void igl::snap_points(
  const Eigen::PlainObjectBase<DerivedC > & C,
  const Eigen::PlainObjectBase<DerivedV > & V,
  Eigen::PlainObjectBase<DerivedI > & I,
  Eigen::PlainObjectBase<DerivedminD > & minD,
  Eigen::PlainObjectBase<DerivedVI > & VI)
{
  snap_points(C,V,I,minD);
  const int m = C.rows();
  VI.resize(m,V.cols());
  for(int c = 0;c<m;c++)
  {
    VI.row(c) = V.row(I(c));
  }
}

template <
  typename DerivedC, 
  typename DerivedV, 
  typename DerivedI, 
  typename DerivedminD>
IGL_INLINE void igl::snap_points(
  const Eigen::PlainObjectBase<DerivedC > & C,
  const Eigen::PlainObjectBase<DerivedV > & V,
  Eigen::PlainObjectBase<DerivedI > & I,
  Eigen::PlainObjectBase<DerivedminD > & minD)
{
  using namespace std;
  const int n = V.rows();
  const int m = C.rows();
  assert(V.cols() == C.cols() && "Dimensions should match");
  // O(m*n)
  //
  // I believe there should be a way to do this in O(m*log(n) + n) assuming
  // reasonably distubed points.
  I.resize(m,1);
  typedef typename DerivedV::Scalar Scalar;
  minD.setConstant(m,1,numeric_limits<Scalar>::max());
  for(int v = 0;v<n;v++)
  {
    for(int c = 0;c<m;c++)
    {
      const Scalar d = (C.row(c) - V.row(v)).squaredNorm();
      if(d < minD(c))
      {
        minD(c,0) = d;
        I(c,0) = v;
      }
    }
  }
}

template <
  typename DerivedC, 
  typename DerivedV, 
  typename DerivedI>
IGL_INLINE void igl::snap_points(
  const Eigen::PlainObjectBase<DerivedC > & C,
  const Eigen::PlainObjectBase<DerivedV > & V,
  Eigen::PlainObjectBase<DerivedI > & I)
{
  Eigen::Matrix<typename DerivedC::Scalar,DerivedC::RowsAtCompileTime,1> minD;
  return igl::snap_points(C,V,I,minD);
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::snap_points<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::snap_points<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif

