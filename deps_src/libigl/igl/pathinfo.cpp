// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "pathinfo.h"

#include "dirname.h"
#include "basename.h"
// Verbose should be removed once everything working correctly
#include "verbose.h"
#include <algorithm>

IGL_INLINE void igl::pathinfo(
  const std::string & path,
  std::string & dirname,
  std::string & basename,
  std::string & extension,
  std::string & filename)
{
  dirname = igl::dirname(path);
  basename = igl::basename(path);
  std::string::reverse_iterator last_dot =
    std::find(
      basename.rbegin(), 
      basename.rend(), '.');
  // Was a dot found?
  if(last_dot == basename.rend())
  {
    // filename is same as basename
    filename = basename;
    // no extension
    extension = "";
  }else
  {
  // extension is substring of basename
    extension = std::string(last_dot.base(),basename.end());
    // filename is substring of basename
    filename = std::string(basename.begin(),last_dot.base()-1);
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
