// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "isdiag.h"

template <typename Scalar>
IGL_INLINE bool igl::isdiag(const Eigen::SparseMatrix<Scalar> & A)
{
  // Iterate over outside of A
  for(int k=0; k<A.outerSize(); ++k)
  {
    // Iterate over inside
    for(typename Eigen::SparseMatrix<Scalar>::InnerIterator it (A,k); it; ++it)
    {
      if(it.row() != it.col() && it.value()!=0)
      {
        return false;
      }
    }
  }
  return true;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::isdiag<double>(class Eigen::SparseMatrix<double,0,int> const &);
#endif
