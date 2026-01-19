// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "EPS.h"

template <> IGL_INLINE float igl::EPS()
{
  return igl::FLOAT_EPS;
}
template <> IGL_INLINE double igl::EPS()
{
  return igl::DOUBLE_EPS;
}

template <> IGL_INLINE float igl::EPS_SQ()
{
  return igl::FLOAT_EPS_SQ;
}
template <> IGL_INLINE double igl::EPS_SQ()
{
  return igl::DOUBLE_EPS_SQ;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
