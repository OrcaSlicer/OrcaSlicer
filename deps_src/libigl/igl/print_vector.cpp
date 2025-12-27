// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "print_vector.h"
#include <iostream>
#include <vector>


template <typename T>
IGL_INLINE void igl::print_vector( std::vector<T>& v)
{
  using namespace std;
  for (int i=0; i<v.size(); ++i)
    std::cerr << v[i] << " ";
  std::cerr << std::endl;
}

template <typename T>
IGL_INLINE void igl::print_vector( std::vector< std::vector<T> >& v)
{
  using namespace std;
  for (int i=0; i<v.size(); ++i)
  {
    std::cerr << i << ": ";
    for (int j=0; j<v[i].size(); ++j)
      std::cerr << v[i][j] << " ";
    std::cerr << std::endl;
  }
}


template <typename T>
IGL_INLINE void igl::print_vector( std::vector< std::vector< std::vector<T> > >& v)
{
  using namespace std;
  for (int m=0; m<v.size(); ++m)
  {
    std::cerr << "Matrix " << m << std::endl;

    for (int i=0; i<v[m].size(); ++i)
    {
      std::cerr << i << ": ";
      for (int j=0; j<v[m][i].size(); ++j)
        std::cerr << v[m][i][j] << " ";
      std::cerr << std::endl;
    }
    
    std::cerr << "---- end " << m << std::endl;

  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
