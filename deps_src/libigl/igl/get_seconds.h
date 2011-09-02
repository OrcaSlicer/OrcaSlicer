// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_GET_SECONDS_H
#define IGL_GET_SECONDS_H
#include "igl_inline.h"

namespace igl
{
  // Return the current time in seconds since program start
  // 
  // Example:
  //    const auto & tictoc = []()
  //    {
  //      static double t_start = igl::get_seconds();
  //      double diff = igl::get_seconds()-t_start;
  //      t_start += diff;
  //      return diff;
  //    };
  //    tictoc();
  //    ... // part 1
  //    cout<<"part 1: "<<tictoc()<<endl;
  //    ... // part 2
  //    cout<<"part 2: "<<tictoc()<<endl;
  //    ... // etc
  IGL_INLINE double get_seconds();

}

#ifndef IGL_STATIC_LIBRARY
#  include "get_seconds.cpp"
#endif

#endif
