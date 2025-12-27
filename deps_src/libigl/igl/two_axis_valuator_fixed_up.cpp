// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "two_axis_valuator_fixed_up.h"
#include "PI.h"

template <typename Scalardown_quat, typename Scalarquat>
IGL_INLINE void igl::two_axis_valuator_fixed_up(
  const int w,
  const int h,
  const double speed,
  const Eigen::Quaternion<Scalardown_quat> & down_quat,
  const int down_x,
  const int down_y,
  const int mouse_x,
  const int mouse_y,
  Eigen::Quaternion<Scalarquat> & quat)
{
  using namespace Eigen;
  Matrix<Scalarquat,3,1> axis(0,1,0);
  quat = down_quat *
    Quaternion<Scalarquat>(
      AngleAxis<Scalarquat>(
        PI*((Scalarquat)(mouse_x-down_x))/(Scalarquat)w*speed/2.0,
        axis.normalized()));
  quat.normalize();
  {
    Matrix<Scalarquat,3,1> axis(1,0,0);
    if(axis.norm() != 0)
    {
      quat =
        Quaternion<Scalarquat>(
          AngleAxis<Scalarquat>(
            PI*(mouse_y-down_y)/(Scalarquat)h*speed/2.0,
            axis.normalized())) * quat;
      quat.normalize();
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::two_axis_valuator_fixed_up<float, float>(int, int, double, Eigen::Quaternion<float, 0> const&, int, int, int, int, Eigen::Quaternion<float, 0>&);
template void igl::two_axis_valuator_fixed_up<double, double>(int, int, double, Eigen::Quaternion<double, 0> const&, int, int, int, int, Eigen::Quaternion<double, 0>&);
#endif
