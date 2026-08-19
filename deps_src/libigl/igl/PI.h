// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PI_H
#define IGL_PI_H
namespace igl
{
  // Use standard mathematical constants' M_PI if available
#ifdef M_PI
  /// π
  constexpr double PI = M_PI;
#else
  /// π
  constexpr double PI = 3.1415926535897932384626433832795;
#endif
}
#endif
