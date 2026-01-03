// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2019 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "iterative_closest_point.h"
#include "AABB.h"
#include "per_face_normals.h"
#include "random_points_on_mesh.h"
#include "placeholders.h"
#include "rigid_alignment.h"
#include <cassert>
#include <iostream>

template <
  typename DerivedVX,
  typename DerivedFX,
  typename DerivedVY,
  typename DerivedFY,
  typename DerivedR,
  typename Derivedt
  >
IGL_INLINE void igl::iterative_closest_point(
  const Eigen::MatrixBase<DerivedVX> & VX,
  const Eigen::MatrixBase<DerivedFX> & FX,
  const Eigen::MatrixBase<DerivedVY> & VY,
  const Eigen::MatrixBase<DerivedFY> & FY,
  const int num_samples,
  const int max_iters,
  Eigen::PlainObjectBase<DerivedR> & R,
  Eigen::PlainObjectBase<Derivedt> & t)
{

  assert(VX.cols() == 3 && "X should be a mesh in 3D");
  assert(VY.cols() == 3 && "Y should be a mesh in 3D");

  typedef typename DerivedVX::Scalar Scalar;
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> MatrixXS;

  // Precompute BVH on Y
  AABB<DerivedVY,3> Ytree;
  Ytree.init(VY,FY);
  MatrixXS NY;
  per_face_normals(VY,FY,NY);
  return iterative_closest_point(
    VX,FX,VY,FY,Ytree,NY,num_samples,max_iters,R,t);
}

template <
  typename DerivedVX,
  typename DerivedFX,
  typename DerivedVY,
  typename DerivedFY,
  typename DerivedNY,
  typename DerivedR,
  typename Derivedt
  >
IGL_INLINE void igl::iterative_closest_point(
  const Eigen::MatrixBase<DerivedVX> & VX,
  const Eigen::MatrixBase<DerivedFX> & FX,
  const Eigen::MatrixBase<DerivedVY> & VY,
  const Eigen::MatrixBase<DerivedFY> & FY,
  const igl::AABB<DerivedVY,3> & Ytree, 
  const Eigen::MatrixBase<DerivedNY> & NY,
  const int num_samples,
  const int max_iters,
  Eigen::PlainObjectBase<DerivedR> & R,
  Eigen::PlainObjectBase<Derivedt> & t)
{
  typedef typename DerivedVX::Scalar Scalar;
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> MatrixXS;
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,1> VectorXS;
  typedef Eigen::Matrix<Scalar,3,3> Matrix3S;
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,3> MatrixX3S;
  typedef Eigen::Matrix<Scalar,1,3> RowVector3S;
  R.setIdentity(3,3);
  t.setConstant(1,3,0);

  for(int iter = 0;iter<max_iters;iter++)
  {
    // Sample random points on X
    MatrixXS X;
    {
      Eigen::VectorXi XI;
      MatrixX3S B;
      MatrixXS VXRT = (VX*R).rowwise()+t;

      random_points_on_mesh(num_samples,VXRT,FX,B,XI,X);
    }
    // Compute closest point
    Eigen::VectorXi I;
    MatrixXS P;
    {
      VectorXS sqrD;
      Ytree.squared_distance(VY,FY,X,sqrD,I,P);
    }
    // Use better normals?
    MatrixXS N = NY(I,igl::placeholders::all);
    //MatrixXS N = (X - P).rowwise().normalized();
    // fit rotation,translation
    Matrix3S Rup;
    RowVector3S tup;
    // Note: Should try out Szymon Rusinkiewicz's new symmetric icp
    rigid_alignment(X,P,N,Rup,tup);
    // update running rigid transformation
    R = (R*Rup).eval();
    t = (t*Rup + tup).eval();
    // Better stopping condition?
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::iterative_closest_point<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
#endif
