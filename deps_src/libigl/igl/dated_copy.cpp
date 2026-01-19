// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "dated_copy.h"
#include "dirname.h"
#include "basename.h"

#include <ctime>
#include <fstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <iostream>

#if !defined(_WIN32)
#include <unistd.h>

IGL_INLINE bool igl::dated_copy(const std::string & src_path, const std::string & dir)
{
  using namespace std;
  // Get time and date as string
  char buffer[80];
  time_t rawtime;
  struct tm * timeinfo;
  time (&rawtime);
  timeinfo = localtime (&rawtime);
  // ISO 8601 format with hyphens instead of colons and no timezone offset
  strftime (buffer,80,"%Y-%m-%dT%H-%M-%S",timeinfo);
  string src_basename = basename(src_path);
  string dst_basename = src_basename+"-"+buffer;
  string dst_path = dir+"/"+dst_basename;
  cerr<<"Saving binary to "<<dst_path;
  {
    // http://stackoverflow.com/a/10195497/148668
    ifstream src(src_path,ios::binary);
    if (!src.is_open()) 
    {
      cerr<<" failed."<<endl;
      return false;
    }
    ofstream dst(dst_path,ios::binary);
    if(!dst.is_open())
    {
      cerr<<" failed."<<endl;
      return false;
    }
    dst << src.rdbuf();
  }
  cerr<<" succeeded."<<endl;
  cerr<<"Setting permissions of "<<dst_path;
  {
    int src_posix = fileno(fopen(src_path.c_str(),"r"));
    if(src_posix == -1)
    {
      cerr<<" failed."<<endl;
      return false;
    }
    struct stat fst;
    fstat(src_posix,&fst);
    int dst_posix = fileno(fopen(dst_path.c_str(),"r"));
    if(dst_posix == -1)
    {
      cerr<<" failed."<<endl;
      return false;
    }
    //update to the same uid/gid
    if(fchown(dst_posix,fst.st_uid,fst.st_gid))
    {
      cerr<<" failed."<<endl;
      return false;
    }
    //update the permissions 
    if(fchmod(dst_posix,fst.st_mode) == -1)
    {
      cerr<<" failed."<<endl;
      return false;
    }
    cerr<<" succeeded."<<endl;
  }
  return true;
}

IGL_INLINE bool igl::dated_copy(const std::string & src_path)
{
  return dated_copy(src_path,dirname(src_path));
}

#endif
