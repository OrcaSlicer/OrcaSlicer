// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_VERBOSE_H
#define IGL_VERBOSE_H

// This function is only useful as a header-only inlined function

namespace igl
{
  // Provide a wrapper for printf, called verbose that functions exactly like
  // printf if VERBOSE is defined and does exactly nothing if VERBOSE is
  // undefined
  inline int verbose(const char * msg,...);
}



#include <cstdio>
#ifdef VERBOSE
#  include <cstdarg>
#endif

#include <string>
// http://channel9.msdn.com/forums/techoff/254707-wrapping-printf-in-c/
#ifdef VERBOSE
inline int igl::verbose(const char * msg,...)
{
  va_list argList;
  va_start(argList, msg);
  int count = vprintf(msg, argList);
  va_end(argList);
  return count;
}
#else
inline int igl::verbose(const char * /*msg*/,...)
{
  return 0;
}
#endif

#endif
