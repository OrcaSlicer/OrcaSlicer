// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "next_filename.h"
#include "STR.h"
#include "file_exists.h"
#include <cmath>
#include <iomanip>

bool igl::next_filename(
  const std::string & prefix, 
  const int zeros,
  const std::string & suffix,
  std::string & next)
{
  using namespace std;
  // O(n), for huge lists could at least find bounds with exponential search
  // and then narrow with binary search O(log(n))
  int i = 0;
  while(true)
  {
    next = STR(prefix << setfill('0') << setw(zeros)<<i<<suffix);
    if(!file_exists(next))
    {
      return true;
    }
    i++;
    if(zeros > 0 && i >= pow(10,zeros))
    {
      return false;
    }
  }
}

