// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "angular_distance.h"
#include "EPS.h"
#include "PI.h"
IGL_INLINE double igl::angular_distance(
  const Eigen::Quaterniond & A,
  const Eigen::Quaterniond & B)
{
  assert(fabs(A.norm()-1)<FLOAT_EPS && "A should be unit norm");
  assert(fabs(B.norm()-1)<FLOAT_EPS && "B should be unit norm");
  //// acos is always in [0,2*pi)
  //return acos(fabs(A.dot(B)));
  return fmod(2.*acos(A.dot(B)),2.*PI);
}
