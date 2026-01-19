// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "is_readable.h"

#ifdef _WIN32
#  include <cstdio>
IGL_INLINE bool igl::is_readable(const char* filename)
{
  FILE * f = fopen(filename,"r");
  if(f == NULL)
  {
    return false;
  }
  fclose(f);
  return true;
}
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  include <iostream>
IGL_INLINE bool igl::is_readable(const char* filename)
{
  // Check if file already exists
  struct stat status;
  if(stat(filename,&status)!=0)
  {
    return false;
  }

  // Get current users uid and gid
  uid_t this_uid = getuid();
  gid_t this_gid = getgid();

  // Dealing with owner
  if( this_uid == status.st_uid )
  {
    return S_IRUSR & status.st_mode;
  }

  // Dealing with group member
  if( this_gid == status.st_gid )
  {
    return S_IRGRP & status.st_mode;
  }

  // Dealing with other
  return S_IROTH & status.st_mode;

}
#endif
