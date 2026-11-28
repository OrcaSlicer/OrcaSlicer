// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DECIMATE_TRIVIAL_CALLBACKS_H
#define IGL_DECIMATE_TRIVIAL_CALLBACKS_H
#include "igl_inline.h"
#include "decimate_callback_types.h"
namespace igl
{
  /// Function to build trivial pre and post collapse actions. 
  ///
  /// @param[out] always_try  function that always returns true (always attempt the next
  ///               edge collapse)
  /// @param[out] never_care  fuction that is always a no-op (never have a post collapse
  ///     response)
  IGL_INLINE void decimate_trivial_callbacks(
    decimate_pre_collapse_callback  & always_try,
    decimate_post_collapse_callback & never_care);
};

#ifndef IGL_STATIC_LIBRARY
#  include "decimate_trivial_callbacks.cpp"
#endif

#endif 

