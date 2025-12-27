// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "file_exists.h"

#include <sys/stat.h>

IGL_INLINE bool igl::file_exists(const std::string filename)
{
  struct stat status;
  return (stat(filename.c_str(),&status)==0);
}
