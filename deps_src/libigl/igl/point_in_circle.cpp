// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "point_in_circle.h"

IGL_INLINE bool igl::point_in_circle(
  const double qx, 
  const double qy,
  const double cx, 
  const double cy,
  const double r)
{
  return (qx-cx)*(qx-cx) + (qy-cy)*(qy-cy) - r*r < 0;
}
