// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "column_to_quats.h"
IGL_INLINE bool igl::column_to_quats(
  const Eigen::VectorXd & Q,
  std::vector<
    Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & vQ)
{
  using namespace Eigen;
  if(Q.size() % 4 != 0)
  {
    return false;
  }
  const int nQ = Q.size()/4;
  vQ.resize(nQ);
  for(int q=0;q<nQ;q++)
  {
    // Constructor uses wxyz
    vQ[q] = Quaterniond( Q(q*4+3), Q(q*4+0), Q(q*4+1), Q(q*4+2));
  }
  return true;
}
