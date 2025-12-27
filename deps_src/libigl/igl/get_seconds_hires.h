// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_GET_SECONDS_HIRES_H
#define IGL_GET_SECONDS_HIRES_H
#include "igl_inline.h"

namespace igl
{
  // Return the current time in seconds using performance counters
  IGL_INLINE double get_seconds_hires();
}

#ifndef IGL_STATIC_LIBRARY
#  include "get_seconds_hires.cpp"
#endif

#endif
