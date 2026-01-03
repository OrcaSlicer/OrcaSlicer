// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "bone_parents.h"

template <typename DerivedBE, typename DerivedP>
IGL_INLINE void igl::bone_parents(
  const Eigen::MatrixBase<DerivedBE>& BE,
  Eigen::PlainObjectBase<DerivedP>& P)
{
  P.resize(BE.rows(),1);
  // Stupid O(n²) version
  for(int e = 0;e<BE.rows();e++)
  {
    P(e) = -1;
    for(int f = 0;f<BE.rows();f++)
    {
      if(BE(e,0) == BE(f,1))
      {
        P(e) = f;
      }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
template void igl::bone_parents<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
