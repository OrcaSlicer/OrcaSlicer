// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CANONICAL_QUATERNIONS_H
#define IGL_CANONICAL_QUATERNIONS_H
#include "igl_inline.h"
// Define some canonical quaternions for floats and doubles
// A Quaternion, q, is defined here as an arrays of four scalars (x,y,z,w),
// such that q = x*i + y*j + z*k + w
namespace igl
{
  // Float versions
#define SQRT_2_OVER_2 0.707106781f
  // Identity
  const float IDENTITY_QUAT_F[4] = {0,0,0,1};
  // The following match the Matlab canonical views
  // X point right, Y pointing up and Z point out
  const float XY_PLANE_QUAT_F[4] = {0,0,0,1};
  // X points right, Y points *in* and Z points up
  const float XZ_PLANE_QUAT_F[4] = {-SQRT_2_OVER_2,0,0,SQRT_2_OVER_2};
  // X points out, Y points right, and Z points up
  const float YZ_PLANE_QUAT_F[4] = {-0.5,-0.5,-0.5,0.5};
  const float CANONICAL_VIEW_QUAT_F[][4] = 
    {
      {             0,             0,             0,             1}, // 0
      {             0,             0, SQRT_2_OVER_2, SQRT_2_OVER_2}, // 1
      {             0,             0,             1,             0}, // 2
      {             0,             0, SQRT_2_OVER_2,-SQRT_2_OVER_2}, // 3

      {             0,            -1,             0,             0}, // 4
      {-SQRT_2_OVER_2, SQRT_2_OVER_2,             0,             0}, // 5
      {            -1,             0,             0,             0}, // 6
      {-SQRT_2_OVER_2,-SQRT_2_OVER_2,             0,             0}, // 7

      {          -0.5,          -0.5,          -0.5,           0.5}, // 8
      {             0,-SQRT_2_OVER_2,             0, SQRT_2_OVER_2}, // 9
      {           0.5,          -0.5,           0.5,           0.5}, // 10
      { SQRT_2_OVER_2,             0, SQRT_2_OVER_2,             0}, // 11

      { SQRT_2_OVER_2,             0,-SQRT_2_OVER_2,             0}, // 12
      {           0.5,           0.5,          -0.5,           0.5}, // 13
      {             0, SQRT_2_OVER_2,             0, SQRT_2_OVER_2}, // 14
      {          -0.5,           0.5,           0.5,           0.5}, // 15

      {             0, SQRT_2_OVER_2, SQRT_2_OVER_2,             0}, // 16
      {          -0.5,           0.5,           0.5,          -0.5}, // 17
      {-SQRT_2_OVER_2,             0,             0,-SQRT_2_OVER_2}, // 18
      {          -0.5,          -0.5,          -0.5,          -0.5}, // 19

      {-SQRT_2_OVER_2,             0,             0, SQRT_2_OVER_2}, // 20
      {          -0.5,          -0.5,           0.5,           0.5}, // 21
      {             0,-SQRT_2_OVER_2, SQRT_2_OVER_2,             0}, // 22
      {           0.5,          -0.5,           0.5,          -0.5}  // 23
    };
#undef SQRT_2_OVER_2
  // Double versions
#define SQRT_2_OVER_2 0.70710678118654757
  // Identity
  const double IDENTITY_QUAT_D[4] = {0,0,0,1};
  // The following match the Matlab canonical views
  // X point right, Y pointing up and Z point out
  const double XY_PLANE_QUAT_D[4] = {0,0,0,1};
  // X points right, Y points *in* and Z points up
  const double XZ_PLANE_QUAT_D[4] = {-SQRT_2_OVER_2,0,0,SQRT_2_OVER_2};
  // X points out, Y points right, and Z points up
  const double YZ_PLANE_QUAT_D[4] = {-0.5,-0.5,-0.5,0.5};
  const double CANONICAL_VIEW_QUAT_D[][4] = 
    {
      {             0,             0,             0,             1},
      {             0,             0, SQRT_2_OVER_2, SQRT_2_OVER_2},
      {             0,             0,             1,             0},
      {             0,             0, SQRT_2_OVER_2,-SQRT_2_OVER_2},
  
      {             0,            -1,             0,             0},
      {-SQRT_2_OVER_2, SQRT_2_OVER_2,             0,             0},
      {            -1,             0,             0,             0},
      {-SQRT_2_OVER_2,-SQRT_2_OVER_2,             0,             0},
  
      {          -0.5,          -0.5,          -0.5,           0.5},
      {             0,-SQRT_2_OVER_2,             0, SQRT_2_OVER_2},
      {           0.5,          -0.5,           0.5,           0.5},
      { SQRT_2_OVER_2,             0, SQRT_2_OVER_2,             0},
  
      { SQRT_2_OVER_2,             0,-SQRT_2_OVER_2,             0},
      {           0.5,           0.5,          -0.5,           0.5},
      {             0, SQRT_2_OVER_2,             0, SQRT_2_OVER_2},
      {          -0.5,           0.5,           0.5,           0.5},
  
      {             0, SQRT_2_OVER_2, SQRT_2_OVER_2,             0},
      {          -0.5,           0.5,           0.5,          -0.5},
      {-SQRT_2_OVER_2,             0,             0,-SQRT_2_OVER_2},
      {          -0.5,          -0.5,          -0.5,          -0.5},
  
      {-SQRT_2_OVER_2,             0,             0, SQRT_2_OVER_2},
      {          -0.5,          -0.5,           0.5,           0.5},
      {             0,-SQRT_2_OVER_2, SQRT_2_OVER_2,             0},
      {           0.5,          -0.5,           0.5,          -0.5}
    };
#undef SQRT_2_OVER_2
#define NUM_CANONICAL_VIEW_QUAT 24

  // NOTE: I want to rather be able to return a Q_type[][] but C++ is not
  // making it easy. So instead I've written a per-element accessor
  
  // Return element [i][j] of the corresponding CANONICAL_VIEW_QUAT_* of the
  // given templated type
  // Inputs:
  //   i  index of quaternion
  //   j  index of coordinate in quaternion i
  // Returns values of CANONICAL_VIEW_QUAT_*[i][j]
  template <typename Q_type> 
  IGL_INLINE Q_type CANONICAL_VIEW_QUAT(int i, int j);
  // Template specializations for float and double
  template <> 
  IGL_INLINE float CANONICAL_VIEW_QUAT<float>(int i, int j);
  template <> 
  IGL_INLINE double CANONICAL_VIEW_QUAT<double>(int i, int j);

}

#ifndef IGL_STATIC_LIBRARY
#  include "canonical_quaternions.cpp"
#endif

#endif
