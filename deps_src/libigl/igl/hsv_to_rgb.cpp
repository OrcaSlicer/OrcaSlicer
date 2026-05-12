// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "hsv_to_rgb.h"
#include <cmath>


template <typename T>
IGL_INLINE void igl::hsv_to_rgb(const T * hsv, T * rgb)
{
  igl::hsv_to_rgb(
      hsv[0],hsv[1],hsv[2],
      rgb[0],rgb[1],rgb[2]);
}

template <typename T>
IGL_INLINE void igl::hsv_to_rgb(
  const T & h, const T & s, const T & v,
  T & r, T & g, T & b)
{
  // From medit
  double f,p,q,t,hh;
  int    i;
  // shift the hue to the range [0, 360] before performing calculations
  hh = ((360 + ((int)h % 360)) % 360) / 60.;
  i = (int)std::floor(hh);    /* largest int <= h     */
  f = hh - i;                    /* fractional part of h */
  p = v * (1.0 - s);
  q = v * (1.0 - (s * f));
  t = v * (1.0 - (s * (1.0 - f)));

  switch(i) {
  case 0: r = v; g = t; b = p; break;
  case 1: r = q; g = v; b = p; break;
  case 2: r = p; g = v; b = t; break;
  case 3: r = p; g = q; b = v; break;
  case 4: r = t; g = p; b = v; break;
  case 5: r = v; g = p; b = q; break;
  }
}

template <typename DerivedH, typename DerivedR>
void igl::hsv_to_rgb(
  const Eigen::MatrixBase<DerivedH> & H,
  Eigen::PlainObjectBase<DerivedR> & R)
{
  assert(H.cols() == 3);
  R.resizeLike(H);
  for(typename DerivedH::Index r = 0;r<H.rows();r++)
  {
    typename DerivedH::Scalar hsv[3];
    hsv[0] = H(r,0);
    hsv[1] = H(r,1);
    hsv[2] = H(r,2);
    typename DerivedR::Scalar rgb[] = {0,0,0};
    hsv_to_rgb(hsv,rgb);
    R(r,0) = rgb[0];
    R(r,1) = rgb[1];
    R(r,2) = rgb[2];
  }
}

#ifdef IGL_STATIC_LIBRARY
template void igl::hsv_to_rgb<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::hsv_to_rgb<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&);
template void igl::hsv_to_rgb<Eigen::Matrix<unsigned char, 64, 3, 1, 64, 3>, Eigen::Matrix<unsigned char, 64, 3, 1, 64, 3> >(Eigen::MatrixBase<Eigen::Matrix<unsigned char, 64, 3, 1, 64, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<unsigned char, 64, 3, 1, 64, 3> >&);
template void igl::hsv_to_rgb<Eigen::Matrix<float, 64, 3, 1, 64, 3>, Eigen::Matrix<float, 64, 3, 1, 64, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, 64, 3, 1, 64, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, 64, 3, 1, 64, 3> >&);
template void igl::hsv_to_rgb<double>(double const*, double*);
#endif
