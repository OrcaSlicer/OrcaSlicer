// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "dot.h"

// http://www.antisphere.com/Wiki/tools:anttweakbar
IGL_INLINE double igl::dot(
  const double *a, 
  const double *b)
{
  return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
