// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "deform_skeleton.h"
void igl::deform_skeleton(
  const Eigen::MatrixXd & C,
  const Eigen::MatrixXi & BE,
  const std::vector<
    Eigen::Affine3d,Eigen::aligned_allocator<Eigen::Affine3d> > & vA,
  Eigen::MatrixXd & CT,
  Eigen::MatrixXi & BET)
{
  using namespace Eigen;
  assert(BE.rows() == (int)vA.size());
  CT.resize(2*BE.rows(),C.cols());
  BET.resize(BE.rows(),2);
  for(int e = 0;e<BE.rows();e++)
  {
    BET(e,0) = 2*e;
    BET(e,1) = 2*e+1;
    Affine3d a = vA[e];
    Vector3d c0 = C.row(BE(e,0));
    Vector3d c1 = C.row(BE(e,1));
    CT.row(2*e) =   a * c0;
    CT.row(2*e+1) = a * c1;
  }

}

IGL_INLINE void igl::deform_skeleton(
  const Eigen::MatrixXd & C,
  const Eigen::MatrixXi & BE,
  const Eigen::MatrixXd & T,
  Eigen::MatrixXd & CT,
  Eigen::MatrixXi & BET)
{
  using namespace Eigen;
  //assert(BE.rows() == (int)vA.size());
  CT.resize(2*BE.rows(),C.cols());
  BET.resize(BE.rows(),2);
  for(int e = 0;e<BE.rows();e++)
  {
    BET(e,0) = 2*e;
    BET(e,1) = 2*e+1;
    Matrix4d t;
    t << T.block(e*4,0,4,3).transpose(), 0,0,0,0;
    Affine3d a;
    a.matrix() = t;
    Vector3d c0 = C.row(BE(e,0));
    Vector3d c1 = C.row(BE(e,1));
    CT.row(2*e) =   a * c0;
    CT.row(2*e+1) = a * c1;
  }
}
