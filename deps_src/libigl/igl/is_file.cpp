// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "is_file.h"

#include <sys/stat.h>
#ifdef _WIN32
#  ifndef S_ISREG
#    define S_ISREG(mode)  (((mode) & S_IFMT) == S_IFREG)
#  endif
#endif
IGL_INLINE bool igl::is_file(const char * filename)
{
  struct stat status;
  if(stat(filename,&status)!=0)
  {
    // path does not exist
    return false;
  }
  // Tests whether existing path is a regular file
  return S_ISREG(status.st_mode);
}
