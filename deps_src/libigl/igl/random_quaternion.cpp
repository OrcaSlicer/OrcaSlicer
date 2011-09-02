// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "random_quaternion.h"
#include "PI.h"

template <typename Scalar>
IGL_INLINE Eigen::Quaternion<Scalar> igl::random_quaternion()
{
  const auto & unit_rand = []()->Scalar
  {
    return ((Scalar)rand() / (Scalar)RAND_MAX);
  };
#ifdef false
  // http://mathproofs.blogspot.com/2005/05/uniformly-distributed-random-unit.html
  const Scalar t0 = 2.*igl::PI*unit_rand();
  const Scalar t1 = acos(1.-2.*unit_rand());
  const Scalar t2 = 0.5*(igl::PI*unit_rand() + acos(unit_rand()));
  return Eigen::Quaternion<Scalar>(
    1.*sin(t0)*sin(t1)*sin(t2),
    1.*cos(t0)*sin(t1)*sin(t2),
    1.*cos(t1)*sin(t2),
    1.*cos(t2));
#elif false
  // "Uniform Random Rotations" [Shoemake 1992] method 1
  const auto & uurand = [&unit_rand]()->Scalar
  {
    return unit_rand()*2.-1.; 
  };
  Scalar x = uurand();
  Scalar y = uurand();
  Scalar z = uurand();
  Scalar w = uurand();
  const auto & hype = [&uurand](Scalar & x, Scalar & y)->Scalar
  {
    Scalar s1;
    while((s1 = x*x + y*y) > 1.0)
    {
      x = uurand();
      y = uurand();
    }
    return s1;
  };
  Scalar s1 = hype(x,y);
  Scalar s2 = hype(z,w);
  Scalar num1 = -2.*log(s1);
  Scalar num2 = -2.*log(s2);
  Scalar r = num1 + num2;
  Scalar root1 = sqrt((num1/s1)/r);
  Scalar root2 = sqrt((num2/s2)/r);
  return Eigen::Quaternion<Scalar>(
    x*root1,
    y*root1,
    z*root2,
    w*root2);
#else
  // Shoemake method 2
  const Scalar x0 = unit_rand();
  const Scalar x1 = unit_rand();
  const Scalar x2 = unit_rand();
  const Scalar r1 = sqrt(1.0 - x0);
  const Scalar r2 = sqrt(x0);
  const Scalar t1 = 2.*igl::PI*x1;
  const Scalar t2 = 2.*igl::PI*x2;
  const Scalar c1 = cos(t1);
  const Scalar s1 = sin(t1);
  const Scalar c2 = cos(t2);
  const Scalar s2 = sin(t2);
  return Eigen::Quaternion<Scalar>(
    s1*r1,
    c1*r1,
    s2*r2,
    c2*r2);
#endif
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template Eigen::Quaternion<double, 0> igl::random_quaternion<double>();
#endif
