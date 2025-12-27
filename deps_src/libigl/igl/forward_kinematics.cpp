// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "forward_kinematics.h"
#include <functional>
#include <iostream>

IGL_INLINE void igl::forward_kinematics(
  const Eigen::MatrixXd & C,
  const Eigen::MatrixXi & BE,
  const Eigen::VectorXi & P,
  const std::vector<
    Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & dQ,
  const std::vector<Eigen::Vector3d> & dT,
  std::vector<
    Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & vQ,
  std::vector<Eigen::Vector3d> & vT)
{
  using namespace std;
  using namespace Eigen;
  const int m = BE.rows(); 
  assert(m == P.rows());
  assert(m == (int)dQ.size());
  assert(m == (int)dT.size());
  vector<bool> computed(m,false);
  vQ.resize(m);
  vT.resize(m);
  // Dynamic programming
  function<void (int) > fk_helper = [&] (int b)
  {
    if(!computed[b])
    {
      if(P(b) < 0)
      {
        // base case for roots
        vQ[b] = dQ[b];
        const Vector3d r = C.row(BE(b,0)).transpose();
        vT[b] = r-dQ[b]*r + dT[b];
      }else
      {
        // Otherwise first compute parent's
        const int p = P(b);
        fk_helper(p);
        vQ[b] = vQ[p] * dQ[b];
        const Vector3d r = C.row(BE(b,0)).transpose();
        vT[b] = vT[p] - vQ[b]*r + vQ[p]*(r + dT[b]);
      }
      computed[b] = true;
    }
  };
  for(int b = 0;b<m;b++)
  {
    fk_helper(b);
  }
}

IGL_INLINE void igl::forward_kinematics(
  const Eigen::MatrixXd & C,
  const Eigen::MatrixXi & BE,
  const Eigen::VectorXi & P,
  const std::vector<
    Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & dQ,
  std::vector<
    Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & vQ,
  std::vector<Eigen::Vector3d> & vT)
{
  std::vector<Eigen::Vector3d> dT(BE.rows(),Eigen::Vector3d(0,0,0));
  return forward_kinematics(C,BE,P,dQ,dT,vQ,vT);
}

IGL_INLINE void igl::forward_kinematics(
  const Eigen::MatrixXd & C,
  const Eigen::MatrixXi & BE,
  const Eigen::VectorXi & P,
  const std::vector<
    Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & dQ,
  const std::vector<Eigen::Vector3d> & dT,
  Eigen::MatrixXd & T)
{
  using namespace Eigen;
  using namespace std;
  vector< Quaterniond,aligned_allocator<Quaterniond> > vQ;
  vector< Vector3d> vT;
  forward_kinematics(C,BE,P,dQ,dT,vQ,vT);
  const int dim = C.cols();
  T.resize(BE.rows()*(dim+1),dim);
  for(int e = 0;e<BE.rows();e++)
  {
    Affine3d a = Affine3d::Identity();
    a.translate(vT[e]);
    a.rotate(vQ[e]);
    T.block(e*(dim+1),0,dim+1,dim) =
      a.matrix().transpose().block(0,0,dim+1,dim);
  }
}

IGL_INLINE void igl::forward_kinematics(
  const Eigen::MatrixXd & C,
  const Eigen::MatrixXi & BE,
  const Eigen::VectorXi & P,
  const std::vector<
    Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & dQ,
  Eigen::MatrixXd & T)
{
  std::vector<Eigen::Vector3d> dT(BE.rows(),Eigen::Vector3d(0,0,0));
  return forward_kinematics(C,BE,P,dQ,dT,T);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
