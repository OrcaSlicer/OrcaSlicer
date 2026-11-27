// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Francisca Gil Ureta <gilureta@cs.nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "segment_segment_intersect.h"

#include <Eigen/Geometry>

template<typename DerivedSource, typename DerivedDir>
IGL_INLINE bool igl::segment_segment_intersect(
  const Eigen::MatrixBase <DerivedSource> &p,
  const Eigen::MatrixBase <DerivedDir> &r,
  const Eigen::MatrixBase <DerivedSource> &q,
  const Eigen::MatrixBase <DerivedDir> &s,
  double &a_t,
  double &a_u,
  double eps
)
{
  // http://stackoverflow.com/questions/563198/how-do-you-detect-where-two-line-segments-intersect
  // Search intersection between two segments
  // p + t*r :  t \in [0,1]
  // q + u*s :  u \in [0,1]

  // p + t * r = q + u * s  // x s
  // t(r x s) = (q - p) x s
  // t = (q - p) x s / (r x s)

  // (r x s) ~ 0 --> directions are parallel, they will never cross
  Eigen::Matrix<typename DerivedDir::Scalar, 1, 3> rxs = r.cross(s);
  if (rxs.norm() <= eps)
    return false;

  int sign;

  double u;
  // u = (q − p) × r / (r × s)
  Eigen::Matrix<typename DerivedDir::Scalar, 1, 3> u1 = (q - p).cross(r);
  sign = ((u1.dot(rxs)) > 0) ? 1 : -1;
  u = u1.norm() / rxs.norm();
  u = u * sign;

  double t;
  // t = (q - p) x s / (r x s)
  Eigen::Matrix<typename DerivedDir::Scalar, 1, 3> t1 = (q - p).cross(s);
  sign = ((t1.dot(rxs)) > 0) ? 1 : -1;
  t = t1.norm() / rxs.norm();
  t = t * sign;

  a_t = t;
  a_u = u;

  if ((u - 1.) > eps || u < -eps)
    return false;

  if ((t - 1.) > eps || t < -eps)
    return false;

  return true;
};

#ifdef IGL_STATIC_LIBRARY
template bool igl::segment_segment_intersect<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, double&, double&, double);
#endif
