// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "example_fun.h"
#include <iostream>

template <typename Printable>
IGL_INLINE bool igl::example_fun(const Printable & input)
{
  using namespace std;
  cout<<"example_fun: "<<input<<endl;
  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::example_fun<double>(const double& input);
template bool igl::example_fun<int>(const int& input);
#endif
