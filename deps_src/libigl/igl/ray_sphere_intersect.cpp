// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "ray_sphere_intersect.h"

template <
  typename Derivedo,
  typename Derivedd,
  typename Derivedc, 
  typename r_type, 
  typename t_type>
IGL_INLINE int igl::ray_sphere_intersect(
  const Eigen::PlainObjectBase<Derivedo> & ao,
  const Eigen::PlainObjectBase<Derivedd> & d,
  const Eigen::PlainObjectBase<Derivedc> & ac,
  r_type r, 
  t_type & t0,
  t_type & t1)
{
  Eigen::Vector3d o = ao-ac;
  // http://wiki.cgsociety.org/index.php/Ray_Sphere_Intersection
  //Compute A, B and C coefficients
  double a = d.dot(d);
  double b = 2 * d.dot(o);
  double c = o.dot(o) - (r * r);

  //Find discriminant
  double disc = b * b - 4 * a * c;
    
  // if discriminant is negative there are no real roots, so return 
  // false as ray misses sphere
  if (disc < 0)
  {
    return 0;
  }

  // compute q as described above
  double distSqrt = sqrt(disc);
  double q;
  if (b < 0)
  {
    q = (-b - distSqrt)/2.0;
  } else
  {
    q = (-b + distSqrt)/2.0;
  }

  // compute t0 and t1
  t0 = q / a;
  double _t1 = c/q;
  if(_t1 == t0)
  {
    return 1;
  }
  t1 = _t1;
  // make sure t0 is smaller than t1
  if (t0 > t1)
  {
    // if t0 is bigger than t1 swap them around
    double temp = t0;
    t0 = t1;
    t1 = temp;
  }
  return 2;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template int igl::ray_sphere_intersect<Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, 3, 1, 0, 3, 1>, double, double>(Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> > const&, double, double&, double&);
#endif
