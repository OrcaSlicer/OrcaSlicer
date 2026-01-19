// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include <igl/sort_vectors_ccw.h>
#include <igl/sort.h>
#include <Eigen/Dense>

template <typename DerivedS, typename DerivedI>
IGL_INLINE void igl::sort_vectors_ccw(
  const Eigen::PlainObjectBase<DerivedS>& P,
  const Eigen::PlainObjectBase<DerivedS>& N,
  Eigen::PlainObjectBase<DerivedI> &order)
{
  int half_degree = P.cols()/3;
  //local frame
  Eigen::Matrix<typename DerivedS::Scalar,1,3> e1 = P.head(3).normalized();
  Eigen::Matrix<typename DerivedS::Scalar,1,3> e3 = N.normalized();
  Eigen::Matrix<typename DerivedS::Scalar,1,3> e2 = e3.cross(e1);

  Eigen::Matrix<typename DerivedS::Scalar,3,3> F; F<<e1.transpose(),e2.transpose(),e3.transpose();

  Eigen::Matrix<typename DerivedS::Scalar,Eigen::Dynamic,1> angles(half_degree,1);
  for (int i=0; i<half_degree; ++i)
  {
    Eigen::Matrix<typename DerivedS::Scalar,1,3> Pl = F.colPivHouseholderQr().solve(P.segment(i*3,3).transpose()).transpose();
//    assert(fabs(Pl(2))/Pl.cwiseAbs().maxCoeff() <1e-5);
    angles[i] = atan2(Pl(1),Pl(0));
  }

  igl::sort( angles, 1, true, angles, order);
  //make sure that the first element is always  at the top
  while (order[0] != 0)
  {
    //do a circshift
    int temp = order[0];
    for (int i =0; i< half_degree-1; ++i)
      order[i] = order[i+1];
    order(half_degree-1) = temp;
  }
}

template <typename DerivedS, typename DerivedI>
IGL_INLINE void igl::sort_vectors_ccw(
  const Eigen::PlainObjectBase<DerivedS>& P,
  const Eigen::PlainObjectBase<DerivedS>& N,
  Eigen::PlainObjectBase<DerivedI> &order,
  Eigen::PlainObjectBase<DerivedS> &sorted)
  {
  int half_degree = P.cols()/3;
  igl::sort_vectors_ccw(P,N,order);
    sorted.resize(1,half_degree*3);
    for (int i=0; i<half_degree; ++i)
      sorted.segment(i*3,3) = P.segment(order[i]*3,3);
  }

template <typename DerivedS, typename DerivedI>
IGL_INLINE void igl::sort_vectors_ccw(
  const Eigen::PlainObjectBase<DerivedS>& P,
  const Eigen::PlainObjectBase<DerivedS>& N,
  Eigen::PlainObjectBase<DerivedI> &order,
  Eigen::PlainObjectBase<DerivedI> &inv_order)
  {
  int half_degree = P.cols()/3;
  igl::sort_vectors_ccw(P,N,order);
    inv_order.resize(half_degree,1);
    for (int i=0; i<half_degree; ++i)
    {
      for (int j=0; j<half_degree; ++j)
        if (order[j] ==i)
        {
          inv_order(i) = j;
          break;
        }
    }
    assert(inv_order[0] == 0);
  }

template <typename DerivedS, typename DerivedI>
IGL_INLINE void igl::sort_vectors_ccw(
  const Eigen::PlainObjectBase<DerivedS>& P,
  const Eigen::PlainObjectBase<DerivedS>& N,
  Eigen::PlainObjectBase<DerivedI> &order,
  Eigen::PlainObjectBase<DerivedS> &sorted,
  Eigen::PlainObjectBase<DerivedI> &inv_order)
{
  int half_degree = P.cols()/3;

  igl::sort_vectors_ccw(P,N,order,inv_order);

  sorted.resize(1,half_degree*3);
  for (int i=0; i<half_degree; ++i)
    sorted.segment(i*3,3) = P.segment(order[i]*3,3);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::sort_vectors_ccw<Eigen::Matrix<double, 1, -1, 1, 1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::sort_vectors_ccw<Eigen::Matrix<double, 1, -1, 1, 1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> >&);
#endif
