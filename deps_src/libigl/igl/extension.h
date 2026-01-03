// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EXTENSION_H
#define IGL_EXTENSION_H
#include "igl_inline.h"

#include <string>

namespace igl
{
  ///  Extract file extension from path.
  ///
  ///  @param[in] path  path with an extension (path/to/foo.obj)
  ///  @return extension without dot (obj)
  ///
  ///  \see pathinfo, basename, dirname
  IGL_INLINE std::string extension( const std::string & path);
}

#ifndef IGL_STATIC_LIBRARY
#  include "extension.cpp"
#endif

#endif

