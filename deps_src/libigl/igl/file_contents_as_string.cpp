// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "file_contents_as_string.h"

#include <fstream>
#include <cstdio>
#include <cassert>

IGL_INLINE bool igl::file_contents_as_string(
  const std::string file_name,
  std::string & content)
{
  std::ifstream ifs(file_name.c_str());
  // Check that opening the stream worked successfully
  if(!ifs.good())
  {
    fprintf(
      stderr,
      "IOError: file_contents_as_string() cannot open %s\n",
      file_name.c_str());
    return false;
  }
  // Stream file contents into string
  content = std::string(
    (std::istreambuf_iterator<char>(ifs)),
    (std::istreambuf_iterator<char>()));
  return true;
}

IGL_INLINE std::string igl::file_contents_as_string(
  const std::string file_name)
{
  std::string content;
#ifndef NDEBUG
  bool ret = 
#endif
    file_contents_as_string(file_name,content);
  assert(ret && "file_contents_as_string failed to read string from file");
  return content;
}
