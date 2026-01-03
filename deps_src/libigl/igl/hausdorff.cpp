// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "hausdorff.h"
#include "point_mesh_squared_distance.h"

template <
  typename DerivedVA,
  typename DerivedFA,
  typename DerivedVB,
  typename DerivedFB,
  typename Scalar>
IGL_INLINE void igl::hausdorff(
  const Eigen::MatrixBase<DerivedVA> & VA,
  const Eigen::MatrixBase<DerivedFA> & FA,
  const Eigen::MatrixBase<DerivedVB> & VB,
  const Eigen::MatrixBase<DerivedFB> & FB,
  Scalar & d)
{
  using namespace Eigen;
  assert(VA.cols() == 3 && "VA should contain 3d points");
  assert(FA.cols() == 3 && "FA should contain triangles");
  assert(VB.cols() == 3 && "VB should contain 3d points");
  assert(FB.cols() == 3 && "FB should contain triangles");
  Matrix<typename DerivedVA::Scalar, Dynamic, 1> sqr_DBA, sqr_DAB;
  Matrix<typename DerivedVA::Index,Dynamic,1> I;
  Matrix<typename DerivedVA::Scalar,Dynamic,3> C;
  point_mesh_squared_distance(VB,VA,FA,sqr_DBA,I,C);
  point_mesh_squared_distance(VA,VB,FB,sqr_DAB,I,C);
  const Scalar dba = sqr_DBA.maxCoeff();
  const Scalar dab = sqr_DAB.maxCoeff();
  d = sqrt(std::max(dba,dab));
}

template <
  typename DerivedV,
  typename Scalar>
IGL_INLINE void igl::hausdorff(
  const Eigen::MatrixBase<DerivedV>& V,
  const std::function<Scalar(const Scalar &,const Scalar &, const Scalar &)> & dist_to_B,
  Scalar & l,
  Scalar & u)
{
  // e  3-long vector of opposite edge lengths
  Eigen::Matrix<typename DerivedV::Scalar,1,3> e;
  // Maximum edge length
  Scalar e_max = 0;
  for(int i=0;i<3;i++)
  {
    e(i) = (V.row((i+1)%3)-V.row((i+2)%3)).norm();
    e_max = std::max(e_max,e(i));
  }
  // Semiperimeter
  const Scalar s = (e(0)+e(1)+e(2))*0.5;
  // Area
  const Scalar A = sqrt(s*(s-e(0))*(s-e(1))*(s-e(2)));
  // Circumradius
  const Scalar R = e(0)*e(1)*e(2)/(4.*A);
  // inradius
  const Scalar r = A/s;
  // Initialize lower bound to ∞
  l = std::numeric_limits<Scalar>::infinity();
  // d  3-long vector of distance from each corner to B
  Eigen::Matrix<typename DerivedV::Scalar,1,3> d;
  Scalar u1 = std::numeric_limits<Scalar>::infinity();
  Scalar u2 = 0;
  for(int i=0;i<3;i++)
  {
    d(i) = dist_to_B(V(i,0),V(i,1),V(i,2));
    // Lower bound is simply the max over vertex distances
    l = std::max(d(i),l);
    // u1 is the minimum of corner distances + maximum adjacent edge
    u1 = std::min(u1,d(i) + std::max(e((i+1)%3),e((i+2)%3)));
    // u2 first takes the maximum over corner distances
    u2 = std::max(u2,d(i));
  }
  // u2 is the distance from the circumcenter/midpoint of obtuse edge plus the
  // largest corner distance
  u2 += (s-r>2.*R ? R : 0.5*e_max);
  u = std::min(u1,u2);
}

#ifdef IGL_STATIC_LIBRARY
template void igl::hausdorff<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, double&);
template void igl::hausdorff<Eigen::Matrix<double, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, std::function<double (double const&, double const&, double const&)> const&, double&, double&);
#endif
