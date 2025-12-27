// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "rgb_to_hsv.h"

template <typename R,typename H>
IGL_INLINE void igl::rgb_to_hsv(const R * rgb, H * hsv)
{
  // http://en.literateprograms.org/RGB_to_HSV_color_space_conversion_%28C%29
  R rgb_max = 0.0;
  R rgb_min = 1.0;
  rgb_max = (rgb[0]>rgb_max?rgb[0]:rgb_max);
  rgb_max = (rgb[1]>rgb_max?rgb[1]:rgb_max);
  rgb_max = (rgb[2]>rgb_max?rgb[2]:rgb_max);
  rgb_min = (rgb[0]<rgb_min?rgb[0]:rgb_min);
  rgb_min = (rgb[1]<rgb_min?rgb[1]:rgb_min);
  rgb_min = (rgb[2]<rgb_min?rgb[2]:rgb_min);
  //hsv[2] = rgb_max;
  hsv[2] = rgb_max;
  if(hsv[2] == 0)
  {
    hsv[0]=hsv[1]=0;
    return;
  }
  // normalize
  R rgb_n[3];
  rgb_n[0] = rgb[0]/hsv[2];
  rgb_n[1] = rgb[1]/hsv[2];
  rgb_n[2] = rgb[2]/hsv[2];
  // Recomput max min?
  rgb_max = 0;
  rgb_max = (rgb_n[0]>rgb_max?rgb_n[0]:rgb_max);
  rgb_max = (rgb_n[1]>rgb_max?rgb_n[1]:rgb_max);
  rgb_max = (rgb_n[2]>rgb_max?rgb_n[2]:rgb_max);
  rgb_min = 1;
  rgb_min = (rgb_n[0]<rgb_min?rgb_n[0]:rgb_min);
  rgb_min = (rgb_n[1]<rgb_min?rgb_n[1]:rgb_min);
  rgb_min = (rgb_n[2]<rgb_min?rgb_n[2]:rgb_min);
  hsv[1] = rgb_max - rgb_min;
  if(hsv[1] == 0)
  {
    hsv[0] = 0;
    return;
  }
  rgb_n[0] = (rgb_n[0] - rgb_min)/(rgb_max - rgb_min);
  rgb_n[1] = (rgb_n[1] - rgb_min)/(rgb_max - rgb_min);
  rgb_n[2] = (rgb_n[2] - rgb_min)/(rgb_max - rgb_min);
  // Recomput max min?
  rgb_max = 0;
  rgb_max = (rgb_n[0]>rgb_max?rgb_n[0]:rgb_max);
  rgb_max = (rgb_n[1]>rgb_max?rgb_n[1]:rgb_max);
  rgb_max = (rgb_n[2]>rgb_max?rgb_n[2]:rgb_max);
  rgb_min = 1;
  rgb_min = (rgb_n[0]<rgb_min?rgb_n[0]:rgb_min);
  rgb_min = (rgb_n[1]<rgb_min?rgb_n[1]:rgb_min);
  rgb_min = (rgb_n[2]<rgb_min?rgb_n[2]:rgb_min);
  if (rgb_max == rgb_n[0]) {
    hsv[0] = 0.0 + 60.0*(rgb_n[1] - rgb_n[2]);
    if (hsv[0] < 0.0) {
      hsv[0] += 360.0;
    }
  } else if (rgb_max == rgb_n[1]) {
    hsv[0] = 120.0 + 60.0*(rgb_n[2] - rgb_n[0]);
  } else /* rgb_max == rgb_n[2] */ {
    hsv[0] = 240.0 + 60.0*(rgb_n[0] - rgb_n[1]);
  }
}


template <typename DerivedR,typename DerivedH>
IGL_INLINE void igl::rgb_to_hsv(
  const Eigen::PlainObjectBase<DerivedR> & R,
  Eigen::PlainObjectBase<DerivedH> & H)
{
  assert(R.cols() == 3);
  H.resizeLike(R);
  for(typename DerivedR::Index r = 0;r<R.rows();r++)
  {
    typename DerivedR::Scalar rgb[3];
    rgb[0] = R(r,0);
    rgb[1] = R(r,1);
    rgb[2] = R(r,2);
    typename DerivedH::Scalar hsv[] = {0,0,0};
    rgb_to_hsv(rgb,hsv);
    H(r,0) = hsv[0];
    H(r,1) = hsv[1];
    H(r,2) = hsv[2];
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::rgb_to_hsv<float, double>(float const*, double*);
template void igl::rgb_to_hsv<double, double>(double const*, double*);
template void igl::rgb_to_hsv<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::rgb_to_hsv<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&);
template void igl::rgb_to_hsv<Eigen::Matrix<float, 64, 3, 1, 64, 3>, Eigen::Matrix<float, 64, 3, 1, 64, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<float, 64, 3, 1, 64, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, 64, 3, 1, 64, 3> >&); 
#endif
