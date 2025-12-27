// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "dqs.h"
#include <Eigen/Geometry>
template <
  typename DerivedV,
  typename DerivedW,
  typename Q,
  typename QAlloc,
  typename T,
  typename DerivedU>
IGL_INLINE void igl::dqs(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedW> & W,
  const std::vector<Q,QAlloc> & vQ,
  const std::vector<T> & vT,
  Eigen::PlainObjectBase<DerivedU> & U)
{
  using namespace std;
  assert(V.rows() <= W.rows());
  assert(W.cols() == (int)vQ.size());
  assert(W.cols() == (int)vT.size());
  // resize output
  U.resizeLike(V);

  // Convert quats + trans into dual parts
  vector<Q> vD(vQ.size());
  for(int c = 0;c<W.cols();c++)
  {
    const Q & q = vQ[c];
    vD[c].w() = -0.5*( vT[c](0)*q.x() + vT[c](1)*q.y() + vT[c](2)*q.z());
    vD[c].x() =  0.5*( vT[c](0)*q.w() + vT[c](1)*q.z() - vT[c](2)*q.y());
    vD[c].y() =  0.5*(-vT[c](0)*q.z() + vT[c](1)*q.w() + vT[c](2)*q.x());
    vD[c].z() =  0.5*( vT[c](0)*q.y() - vT[c](1)*q.x() + vT[c](2)*q.w());
  }

  // Loop over vertices
  const int nv = V.rows();
#pragma omp parallel for if (nv>10000)
  for(int i = 0;i<nv;i++)
  {
    Q b0(0,0,0,0);
    Q be(0,0,0,0);
    // Loop over handles
    for(int c = 0;c<W.cols();c++)
    {
      b0.coeffs() += W(i,c) * vQ[c].coeffs();
      be.coeffs() += W(i,c) * vD[c].coeffs();
    }
    Q ce = be;
    ce.coeffs() /= b0.norm();
    Q c0 = b0;
    c0.coeffs() /= b0.norm();
    // See algorithm 1 in "Geometric skinning with approximate dual quaternion
    // blending" by Kavan et al
    T v = V.row(i);
    T d0 = c0.vec();
    T de = ce.vec();
    typename Q::Scalar a0 = c0.w();
    typename Q::Scalar ae = ce.w();
    U.row(i) =  v + 2*d0.cross(d0.cross(v) + a0*v) + 2*(a0*de - ae*d0 + d0.cross(de));
  }

}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::dqs<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Quaternion<double, 0>, Eigen::aligned_allocator<Eigen::Quaternion<double, 0> >, Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, std::vector<Eigen::Quaternion<double, 0>, Eigen::aligned_allocator<Eigen::Quaternion<double, 0> > > const&, std::vector<Eigen::Matrix<double, 3, 1, 0, 3, 1>, std::allocator<Eigen::Matrix<double, 3, 1, 0, 3, 1> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
