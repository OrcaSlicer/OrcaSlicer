// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "in_element.h"
#include "parallel_for.h"
template <
  typename DerivedV, 
  typename DerivedEle,
  typename DerivedQ, 
  int DIM,
  typename DerivedI
  >
IGL_INLINE void igl::in_element(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const Eigen::MatrixBase<DerivedQ> & Q,
  const AABB<DerivedV,DIM> & aabb,
  Eigen::PlainObjectBase<DerivedI> & I)
{
  using namespace std;
  using namespace Eigen;
  const int Qr = Q.rows();
  I.setConstant(Qr,1,-1);
  parallel_for(Qr,[&](const int e)
  {
    // find all
    const auto R = aabb.find(V,Ele,Q.row(e).eval(),true);
    if(!R.empty())
    {
      I(e) = R[0];
    }
  },10000);
}

template <typename DerivedV, typename  DerivedEle, typename DerivedQ, int DIM, typename Scalar>
IGL_INLINE void igl::in_element(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const Eigen::MatrixBase<DerivedQ> & Q,
  const AABB<DerivedV,DIM> & aabb,
  Eigen::SparseMatrix<Scalar> & I)
{
  using namespace std;
  using namespace Eigen;
  const int Qr = Q.rows();
  std::vector<Triplet<Scalar> > IJV;
  IJV.reserve(Qr);
// #pragma omp parallel for if (Qr>10000)
  for(int e = 0;e<Qr;e++)
  {
    // find all
    const auto R = aabb.find(V,Ele,Q.row(e).eval(),false);
    for(const auto r : R)
    {
// #pragma omp critical
      IJV.push_back(Triplet<Scalar>(e,r,1));
    }
  }
  I.resize(Qr,Ele.rows());
  I.setFromTriplets(IJV.begin(),IJV.end());
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
  template 
  void igl::in_element
  <
    Eigen::MatrixXd, 
    Eigen::MatrixXi,
    Eigen::MatrixXd, 
    3,
    Eigen::VectorXi>
    (
    const Eigen::MatrixBase<Eigen::MatrixXd> & V,
    const Eigen::MatrixBase<Eigen::MatrixXi> & Ele,
    const Eigen::MatrixBase<Eigen::MatrixXd> & Q,
    const AABB<Eigen::MatrixXd,3> & aabb,
    Eigen::PlainObjectBase<Eigen::VectorXi> & I);
#endif
