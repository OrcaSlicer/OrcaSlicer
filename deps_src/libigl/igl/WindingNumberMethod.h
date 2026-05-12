// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_WINDINGNUMBERMETHOD_H
#define IGL_WINDINGNUMBERMETHOD_H
namespace igl
{
  enum WindingNumberMethod
  {
    // exact hierarchical evaluation
    EXACT_WINDING_NUMBER_METHOD = 0,
    // poor approximation
    APPROX_SIMPLE_WINDING_NUMBER_METHOD = 1,
    // another poor approximation
    APPROX_CACHE_WINDING_NUMBER_METHOD = 2,
    /// Number of winding number methods
    NUM_WINDING_NUMBER_METHODS = 3
  };
}
#endif
