// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "parallel_transport_angles.h"
#include <Eigen/Geometry>

template <typename DerivedV, typename DerivedF, typename DerivedK>
IGL_INLINE void igl::parallel_transport_angles(
const Eigen::MatrixBase<DerivedV>& V,
const Eigen::MatrixBase<DerivedF>& F,
const Eigen::MatrixBase<DerivedV>& FN,
const Eigen::MatrixXi &E2F,
const Eigen::MatrixXi &F2E,
Eigen::PlainObjectBase<DerivedK> &K)
{
  int numE = E2F.rows();

  Eigen::VectorXi isBorderEdge;
  isBorderEdge.setZero(numE,1);
  for(unsigned i=0; i<numE; ++i)
  {
    if ((E2F(i,0) == -1) || ((E2F(i,1) == -1)))
      isBorderEdge[i] = 1;
  }

  K.setZero(numE);
  // For every non-border edge
  for (unsigned eid=0; eid<numE; ++eid)
  {
    if (!isBorderEdge[eid])
    {
      int fid0 = E2F(eid,0);
      int fid1 = E2F(eid,1);

      Eigen::Matrix<typename DerivedV::Scalar, 1, 3> N0 = FN.row(fid0);
//      Eigen::Matrix<typename DerivedV::Scalar, 1, 3> N1 = FN.row(fid1);

      // find common edge on triangle 0 and 1
      int fid0_vc = -1;
      int fid1_vc = -1;
      for (unsigned i=0;i<3;++i)
      {
        if (F2E(fid0,i) == eid)
          fid0_vc = i;
        if (F2E(fid1,i) == eid)
          fid1_vc = i;
      }
      assert(fid0_vc != -1);
      assert(fid1_vc != -1);

      Eigen::Matrix<typename DerivedV::Scalar, 1, 3> common_edge = V.row(F(fid0,(fid0_vc+1)%3)) - V.row(F(fid0,fid0_vc));
      common_edge.normalize();

      // Map the two triangles in a new space where the common edge is the x axis and the N0 the z axis
      Eigen::Matrix<typename DerivedV::Scalar, 3, 3> P;
      Eigen::Matrix<typename DerivedV::Scalar, 1, 3> o = V.row(F(fid0,fid0_vc));
      Eigen::Matrix<typename DerivedV::Scalar, 1, 3> tmp = -N0.cross(common_edge);
      P << common_edge, tmp, N0;
      //      P.transposeInPlace();


      Eigen::Matrix<typename DerivedV::Scalar, 3, 3> V0;
      V0.row(0) = V.row(F(fid0,0)) -o;
      V0.row(1) = V.row(F(fid0,1)) -o;
      V0.row(2) = V.row(F(fid0,2)) -o;

      V0 = (P*V0.transpose()).transpose();

      //      assert(V0(0,2) < 1e-10);
      //      assert(V0(1,2) < 1e-10);
      //      assert(V0(2,2) < 1e-10);

      Eigen::Matrix<typename DerivedV::Scalar, 3, 3> V1;
      V1.row(0) = V.row(F(fid1,0)) -o;
      V1.row(1) = V.row(F(fid1,1)) -o;
      V1.row(2) = V.row(F(fid1,2)) -o;
      V1 = (P*V1.transpose()).transpose();

      //      assert(V1(fid1_vc,2) < 10e-10);
      //      assert(V1((fid1_vc+1)%3,2) < 10e-10);

      // compute rotation R such that R * N1 = N0
      // i.e. map both triangles to the same plane
      double alpha = -atan2(V1((fid1_vc+2)%3,2),V1((fid1_vc+2)%3,1));

      Eigen::Matrix<typename DerivedV::Scalar, 3, 3> R;
      R << 1,          0,            0,
      0, cos(alpha), -sin(alpha) ,
      0, sin(alpha),  cos(alpha);
      V1 = (R*V1.transpose()).transpose();

      //      assert(V1(0,2) < 1e-10);
      //      assert(V1(1,2) < 1e-10);
      //      assert(V1(2,2) < 1e-10);

      // measure the angle between the reference frames
      // k_ij is the angle between the triangle on the left and the one on the right
      Eigen::Matrix<typename DerivedV::Scalar, 1, 3> ref0 = V0.row(1) - V0.row(0);
      Eigen::Matrix<typename DerivedV::Scalar, 1, 3> ref1 = V1.row(1) - V1.row(0);

      ref0.normalize();
      ref1.normalize();

      double ktemp = atan2(ref1(1),ref1(0)) - atan2(ref0(1),ref0(0));

      // just to be sure, rotate ref0 using angle ktemp...
      Eigen::Matrix<typename DerivedV::Scalar,2,2> R2;
      R2 << cos(ktemp), -sin(ktemp), sin(ktemp), cos(ktemp);

//      Eigen::Matrix<typename DerivedV::Scalar, 1, 2> tmp1 = R2*(ref0.head(2)).transpose();

      //      assert(tmp1(0) - ref1(0) < 1e-10);
      //      assert(tmp1(1) - ref1(1) < 1e-10);

      K[eid] = ktemp;
    }
  }

}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::parallel_transport_angles<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1> const&, Eigen::Matrix<int, -1, -1, 0, -1, -1> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
#endif
