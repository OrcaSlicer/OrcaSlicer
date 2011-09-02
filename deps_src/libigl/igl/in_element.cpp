// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "in_element.h"

template <typename DerivedV, typename DerivedQ, int DIM>
IGL_INLINE void igl::in_element(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::MatrixXi & Ele,
  const Eigen::PlainObjectBase<DerivedQ> & Q,
  const AABB<DerivedV,DIM> & aabb,
  Eigen::VectorXi & I)
{
  using namespace std;
  using namespace Eigen;
  const int Qr = Q.rows();
  I.setConstant(Qr,1,-1);
#pragma omp parallel for if (Qr>10000)
  for(int e = 0;e<Qr;e++)
  {
    // find all
    const auto R = aabb.find(V,Ele,Q.row(e).eval(),true);
    if(!R.empty())
    {
      I(e) = R[0];
    }
  }
}

template <typename DerivedV, typename DerivedQ, int DIM, typename Scalar>
IGL_INLINE void igl::in_element(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::MatrixXi & Ele,
  const Eigen::PlainObjectBase<DerivedQ> & Q,
  const AABB<DerivedV,DIM> & aabb,
  Eigen::SparseMatrix<Scalar> & I)
{
  using namespace std;
  using namespace Eigen;
  const int Qr = Q.rows();
  std::vector<Triplet<Scalar> > IJV;
  IJV.reserve(Qr);
#pragma omp parallel for if (Qr>10000)
  for(int e = 0;e<Qr;e++)
  {
    // find all
    const auto R = aabb.find(V,Ele,Q.row(e).eval(),false);
    for(const auto r : R)
    {
#pragma omp critical
      IJV.push_back(Triplet<Scalar>(e,r,1));
    }
  }
  I.resize(Qr,Ele.rows());
  I.setFromTriplets(IJV.begin(),IJV.end());
}

#ifdef IGL_STATIC_LIBRARY
template void igl::in_element<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2> const&, Eigen::Matrix<int, -1, 1, 0, -1, 1>&);
template void igl::in_element<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3> const&, Eigen::Matrix<int, -1, 1, 0, -1, 1>&);
#endif
