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

template <
  typename DerivedF,
  typename DerivedE,
  typename DerivedEMAP,
  typename DerivedEF,
  typename DerivedEI>
IGL_INLINE bool igl::edge_collapse_is_valid(
  const int e,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedE> & E,
  const Eigen::MatrixBase<DerivedEMAP> & EMAP,
  const Eigen::MatrixBase<DerivedEF> & EF,
  const Eigen::MatrixBase<DerivedEI> & EI)
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
    const auto neighbors = [&F,&E,&EMAP,&EF,&EI](
      const int e,
      const bool ccw)
    {
      vector<int> N,uN;
      vector<int> V2Fe = circulation(e, ccw,EMAP,EF,EI);
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
    VectorXi Ns = neighbors(e, eflip);
    VectorXi Nd = neighbors(e,!eflip);
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

IGL_INLINE bool igl::edge_collapse_is_valid(
  std::vector<int> & Nsv,
  std::vector<int> & Ndv)
{
  // Do we really need to check if edge is IGL_COLLAPSE_EDGE_NULL ?

  if(Nsv.size()<2 || Ndv.size()<2)
  {
    // Bogus data
    assert(false);
    return false;
  }
  // determine if the first two vertices are the same before reordering.
  // If they are and there are 3 each, then (I claim) this is an edge on a
  // single tet.
  const bool first_two_same = (Nsv[0] == Ndv[0]) && (Nsv[1] == Ndv[1]);
  if(Nsv.size() == 3 && Ndv.size() == 3 && first_two_same)
  {
    // single tet
    return false;
  }
  // https://stackoverflow.com/a/19483741/148668
  std::sort(Nsv.begin(), Nsv.end());
  std::sort(Ndv.begin(), Ndv.end());
  std::vector<int> Nint;
  std::set_intersection(
    Nsv.begin(), Nsv.end(), Ndv.begin(), Ndv.end(), std::back_inserter(Nint));
  // check if edge collapse is valid: intersection of vertex neighbors of s and
  // d should be exactly 2+(s,d) = 4
  // http://stackoverflow.com/a/27049418/148668
  if(Nint.size() != 2)
  {
    return false;
  }
  
  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
