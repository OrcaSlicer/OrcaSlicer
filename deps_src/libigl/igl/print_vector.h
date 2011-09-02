// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PRINT_VECTOR_H
#define IGL_PRINT_VECTOR_H
#include "igl_inline.h"

#include <vector>
namespace igl 
{
  // Not clear what these are supposed to be doing. Currently they print
  // vectors to standard error...
  template <typename T>
  IGL_INLINE void print_vector( std::vector<T>& v);
  template <typename T>
  IGL_INLINE void print_vector( std::vector< std::vector<T> >& v);
  template <typename T>
  IGL_INLINE void print_vector(std::vector< std::vector< std::vector<T> > >& v);
}

#ifndef IGL_STATIC_LIBRARY
#  include "print_vector.cpp"
#endif

#endif
