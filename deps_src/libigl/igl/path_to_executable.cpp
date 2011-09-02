// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "path_to_executable.h"
#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif
#if defined(_WIN32)
#  include <windows.h>
#endif
#include <stdint.h>
IGL_INLINE std::string igl::path_to_executable()
{
  // http://pastebin.com/ffzzxPzi
  using namespace std;
  std::string path;
  char buffer[1024];
  uint32_t size = sizeof(buffer);
#if defined (WIN32)
  GetModuleFileName(nullptr,buffer,size);
  path = buffer;
#elif defined (__APPLE__)
  if(_NSGetExecutablePath(buffer, &size) == 0)
  {
    path = buffer;
  }
#elif defined(UNIX)
  if (readlink("/proc/self/exe", buffer, sizeof(buffer)) == -1)
  {
    path = buffer;
  }
#elif defined(__FreeBSD__)
  int mib[4];
  mib[0] = CTL_KERN;
  mib[1] = KERN_PROC;
  mib[2] = KERN_PROC_PATHNAME;
  mib[3] = -1;
  sysctl(mib, 4, buffer, sizeof(buffer), NULL, 0);
  path = buffer;
#elif defined(SUNOS)
  path = getexecname();
#endif
  return path;
}

