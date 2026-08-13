// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ONE_H
#define IGL_ONE_H
namespace igl
{
  /// Often one needs a reference to a dummy variable containing one as its
  /// value, for example when using AntTweakBar's
  /// TwSetParam( "3D View", "opened", TW_PARAM_INT32, 1, &INT_ONE);
  const char CHAR_ONE = 1;
  const int INT_ONE = 1;
  const unsigned int UNSIGNED_INT_ONE = 1;
  const double DOUBLE_ONE = 1;
  const float FLOAT_ONE = 1;
}
#endif

