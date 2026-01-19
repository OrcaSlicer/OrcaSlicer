// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "quats_to_column.h"

IGL_INLINE void igl::quats_to_column(
  const std::vector<
    Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > vQ,
    Eigen::VectorXd & Q)
{
  Q.resize(vQ.size()*4);
  for(int q = 0;q<(int)vQ.size();q++)
  {
    auto & xyzw = vQ[q].coeffs();
    for(int c = 0;c<4;c++)
    {
      Q(q*4+c) = xyzw(c);
    }
  }
}

IGL_INLINE Eigen::VectorXd igl::quats_to_column(
  const std::vector<
    Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > vQ)
{
  Eigen::VectorXd Q;
  quats_to_column(vQ,Q);
  return Q;
}
