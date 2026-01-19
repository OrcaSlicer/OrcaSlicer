// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "canonical_quaternions.h"

template <> IGL_INLINE float igl::CANONICAL_VIEW_QUAT<float>(int i, int j)
{
  return (float)igl::CANONICAL_VIEW_QUAT_F[i][j];
}
template <> IGL_INLINE double igl::CANONICAL_VIEW_QUAT<double>(int i, int j)
{
  return (double)igl::CANONICAL_VIEW_QUAT_D[i][j];
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
