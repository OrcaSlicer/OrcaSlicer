// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EMBREE_EMBREE_CONVENIENCE_H
#define IGL_EMBREE_EMBREE_CONVENIENCE_H

#undef interface
#undef near
#undef far
// Why are these in quotes? isn't that a bad idea?
#ifdef __GNUC__
// This is how it should be done
#  if __GNUC__ >= 4
#    if __GNUC_MINOR__ >= 6
#      pragma GCC diagnostic push
#      pragma GCC diagnostic ignored "-Weffc++"
#    endif
#  endif
// This is a hack
#  pragma GCC system_header
#endif
#include <embree/include/embree.h>
#include <embree/include/intersector1.h>
#include <embree/common/ray.h>
#ifdef __GNUC__
#  if __GNUC__ >= 4
#    if __GNUC_MINOR__ >= 6
#      pragma GCC diagnostic pop
#    endif
#  endif
#endif

#endif
