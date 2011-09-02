// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "unzip_corners.h"

#include "unique_rows.h"
#include "slice.h"

template < typename DerivedA, typename DerivedU, typename DerivedG, typename DerivedJ >
IGL_INLINE void igl::unzip_corners(
  const std::vector<std::reference_wrapper<DerivedA> > & A,
  Eigen::PlainObjectBase<DerivedU> & U,
  Eigen::PlainObjectBase<DerivedG> & G,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  if(A.size() == 0)
  {
    U.resize(0,0);
    G.resize(0,3);
    J.resize(0,0);
    return;
  }
  const size_t num_a = A.size();
  const typename DerivedA::Index m = A[0].get().rows();
  DerivedU C(m*3,num_a);
  for(int a = 0;a<num_a;a++)
  {
    assert(A[a].get().rows() == m && "All attributes should be same size");
    assert(A[a].get().cols() == 3 && "Must be triangles");
    C.block(0*m,a,m,1) = A[a].get().col(0);
    C.block(1*m,a,m,1) = A[a].get().col(1);
    C.block(2*m,a,m,1) = A[a].get().col(2);
  }
  DerivedJ I;
  igl::unique_rows(C,U,I,J);
  G.resize(m,3);
  for(int f = 0;f<m;f++)
  {
    for(int c = 0;c<3;c++)
    {
      G(f,c) = J(f+c*m);
    }
  }
}
