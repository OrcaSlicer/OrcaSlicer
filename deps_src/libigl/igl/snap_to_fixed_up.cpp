// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "snap_to_fixed_up.h"

template <typename Qtype>
IGL_INLINE void igl::snap_to_fixed_up(
  const Eigen::Quaternion<Qtype> & q,
  Eigen::Quaternion<Qtype> & s)
{
  using namespace Eigen;
  typedef Eigen::Matrix<Qtype,3,1> Vector3Q;
  const Vector3Q up = q.matrix() * Vector3Q(0,1,0);
  Vector3Q proj_up(0,up(1),up(2));
  if(proj_up.norm() == 0)
  {
    proj_up = Vector3Q(0,1,0);
  }
  proj_up.normalize();
  Quaternion<Qtype> dq;
  dq = Quaternion<Qtype>::FromTwoVectors(up,proj_up);
  s = dq * q;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiations
template void igl::snap_to_fixed_up<float>(Eigen::Quaternion<float, 0> const&, Eigen::Quaternion<float, 0>&);
template void igl::snap_to_fixed_up<double>(Eigen::Quaternion<double, 0> const&, Eigen::Quaternion<double, 0>&);
#endif
