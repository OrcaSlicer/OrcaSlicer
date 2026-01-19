// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "cylinder.h"
#include "PI.h"
#include <cassert>
#include <cmath>

template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::cylinder(
  const int axis_devisions,
  const int height_devisions,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F)
{
  V.resize(axis_devisions*height_devisions,3);
  F.resize(2*(axis_devisions*(height_devisions-1)),3);
  int f = 0;
  typedef typename DerivedV::Scalar Scalar;
  for(int th = 0;th<axis_devisions;th++)
  {
    Scalar x = cos(2.*igl::PI*Scalar(th)/Scalar(axis_devisions));
    Scalar y = sin(2.*igl::PI*Scalar(th)/Scalar(axis_devisions));
    for(int h = 0;h<height_devisions;h++)
    {
      Scalar z = Scalar(h)/Scalar(height_devisions-1);
      V(th+h*axis_devisions,0) = x;
      V(th+h*axis_devisions,1) = y;
      V(th+h*axis_devisions,2) = z;
      if(h > 0)
      {
        F(f,0) = ((th+0)%axis_devisions)+(h-1)*axis_devisions;
        F(f,1) = ((th+1)%axis_devisions)+(h-1)*axis_devisions;
        F(f,2) = ((th+0)%axis_devisions)+(h+0)*axis_devisions;
        f++;
        F(f,0) = ((th+1)%axis_devisions)+(h-1)*axis_devisions;
        F(f,1) = ((th+1)%axis_devisions)+(h+0)*axis_devisions;
        F(f,2) = ((th+0)%axis_devisions)+(h+0)*axis_devisions;
        f++;
      }
    }
  }
  assert(f == F.rows());
}

#ifdef IGL_STATIC_LIBRARY
template void igl::cylinder<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
