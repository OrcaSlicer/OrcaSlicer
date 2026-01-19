// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "edge_collapse_is_valid.h"
#include "collapse_edge.h"
#include "circulation.h"
#include "intersect.h"
#include "unique.h"
#include "list_to_matrix.h"
#include <vector>

IGL_INLINE bool igl::edge_collapse_is_valid(
  const int e,
  const Eigen::MatrixXi & F,
  const Eigen::MatrixXi & E,
  const Eigen::VectorXi & EMAP,
  const Eigen::MatrixXi & EF,
  const Eigen::MatrixXi & EI)
{
  using namespace Eigen;
  using namespace std;
  // For consistency with collapse_edge.cpp, let's determine edge flipness
  // (though not needed to check validity)
  const int eflip = E(e,0)>E(e,1);
  // source and destination
  const int s = eflip?E(e,1):E(e,0);
  const int d = eflip?E(e,0):E(e,1);

  if(s == IGL_COLLAPSE_EDGE_NULL && d==IGL_COLLAPSE_EDGE_NULL)
  {
    return false;
  }
  // check if edge collapse is valid: intersection of vertex neighbors of s and
  // d should be exactly 2+(s,d) = 4
  // http://stackoverflow.com/a/27049418/148668
  {
    // all vertex neighbors around edge, including the two vertices of the edge
    const auto neighbors = [](
      const int e,
      const bool ccw,
      const Eigen::MatrixXi & F,
      const Eigen::MatrixXi & E,
      const Eigen::VectorXi & EMAP,
      const Eigen::MatrixXi & EF,
      const Eigen::MatrixXi & EI) 
    {
      vector<int> N,uN;
      vector<int> V2Fe = circulation(e, ccw,F,E,EMAP,EF,EI);
      for(auto f : V2Fe)
      {
        N.push_back(F(f,0));
        N.push_back(F(f,1));
        N.push_back(F(f,2));
      }
      vector<size_t> _1,_2;
      igl::unique(N,uN,_1,_2);
      VectorXi uNm;
      list_to_matrix(uN,uNm);
      return uNm;
    };
    VectorXi Ns = neighbors(e, eflip,F,E,EMAP,EF,EI);
    VectorXi Nd = neighbors(e,!eflip,F,E,EMAP,EF,EI);
    VectorXi Nint = igl::intersect(Ns,Nd);
    if(Nint.size() != 4)
    {
      return false;
    }
    if(Ns.size() == 4 && Nd.size() == 4)
    {
      VectorXi NsNd(8);
      NsNd<<Ns,Nd;
      VectorXi Nun,_1,_2;
      igl::unique(NsNd,Nun,_1,_2);
      // single tet, don't collapse
      if(Nun.size() == 4)
      {
        return false;
      }
    }
  }
  return true;
}
