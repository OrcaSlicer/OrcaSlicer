// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "circulation.h"
#include "list_to_matrix.h"

IGL_INLINE std::vector<int> igl::circulation(
  const int e,
  const bool ccw,
  const Eigen::MatrixXi & F,
  const Eigen::MatrixXi & E,
  const Eigen::VectorXi & EMAP,
  const Eigen::MatrixXi & EF,
  const Eigen::MatrixXi & EI)
{
  // prepare output
  std::vector<int> N;
  N.reserve(6);
  const int m = F.rows();
  const auto & step = [&](
    const int e, 
    const int ff,
    int & ne, 
    int & nf)
  {
    assert((EF(e,1) == ff || EF(e,0) == ff) && "e should touch ff");
    //const int fside = EF(e,1)==ff?1:0;
    const int nside = EF(e,0)==ff?1:0;
    const int nv = EI(e,nside);
    // get next face
    nf = EF(e,nside);
    // get next edge 
    const int dir = ccw?-1:1;
    ne = EMAP(nf+m*((nv+dir+3)%3));
  };
  // Always start with first face (ccw in step will be sure to turn right
  // direction)
  const int f0 = EF(e,0);
  int fi = f0;
  int ei = e;
  while(true)
  {
    step(ei,fi,ei,fi);
    N.push_back(fi);
    // back to start?
    if(fi == f0)
    {
      assert(ei == e);
      break;
    }
  }
  return N;
}

IGL_INLINE void igl::circulation(
  const int e,
  const bool ccw,
  const Eigen::MatrixXi & F,
  const Eigen::MatrixXi & E,
  const Eigen::VectorXi & EMAP,
  const Eigen::MatrixXi & EF,
  const Eigen::MatrixXi & EI,
  Eigen::VectorXi & vN)
{
  std::vector<int> N = circulation(e,ccw,F,E,EMAP,EF,EI);
  igl::list_to_matrix(N,vN);
}
