// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "get_seconds.h"

#if _WIN32
// Alec: Why is this more "hires" than chrono?
#  include <windows.h>
#  include <cassert>
IGL_INLINE double igl::get_seconds()
{
  LARGE_INTEGER li_freq, li_current;
  const bool ret = QueryPerformanceFrequency(&li_freq);
  const bool ret2 = QueryPerformanceCounter(&li_current);
  assert(ret && ret2);
  assert(li_freq.QuadPart > 0);
  return double(li_current.QuadPart) / double(li_freq.QuadPart);
}
#else
#include <chrono>
IGL_INLINE double igl::get_seconds()
{
  return 
    std::chrono::duration<double>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
#endif
