// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "is_vertex_manifold.h"
#include "triangle_triangle_adjacency.h"
#include "vertex_triangle_adjacency.h"
#include "unique.h"
#include <vector>
#include <cassert>
#include <map>
#include <queue>
#include <iostream>

template <typename DerivedF,typename DerivedB>
IGL_INLINE bool igl::is_vertex_manifold(
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedB>& B)
{
  using namespace std;
  using namespace Eigen;
  assert(F.cols() == 3 && "F must contain triangles");
  typedef typename DerivedF::Scalar Index;
  typedef typename DerivedF::Index FIndex;
  const Index n = F.maxCoeff()+1;
  vector<vector<vector<FIndex > > > TT;
  vector<vector<vector<FIndex > > > TTi;
  triangle_triangle_adjacency(F,TT,TTi);

  vector<vector<FIndex > > V2F,_1;
  vertex_triangle_adjacency(n,F,V2F,_1);

  const auto & check_vertex = [&](const Index v)->bool
  {
    vector<FIndex> uV2Fv;
    {
      vector<size_t> _1,_2;
      unique(V2F[v],uV2Fv,_1,_2);
    }
    const FIndex one_ring_size = uV2Fv.size();
    if(one_ring_size == 0)
    {
      return false;
    }
    const FIndex g = uV2Fv[0];
    queue<Index> Q;
    Q.push(g);
    map<FIndex,bool> seen;
    while(!Q.empty())
    {
      const FIndex f = Q.front();
      Q.pop();
      if(seen.count(f)==1)
      {
        continue;
      }
      seen[f] = true;
      // Face f's neighbor lists opposite opposite each corner
      for(const auto & c : TT[f])
      {
        // Each neighbor
        for(const auto & n : c)
        {
          bool contains_v = false;
          for(Index nc = 0;nc<F.cols();nc++)
          {
            if(F(n,nc) == v)
            {
              contains_v = true;
              break;
            }
          }
          if(seen.count(n)==0 && contains_v)
          {
            Q.push(n);
          }
        }
      }
    }
    return one_ring_size == (FIndex) seen.size();
  };

  // Unreferenced vertices are considered non-manifold
  B.setConstant(n,1,false);
  // Loop over all vertices touched by F
  bool all = true;
  for(Index v = 0;v<n;v++)
  {
    all &= B(v) = check_vertex(v);
  }
  return all;
}

template <typename DerivedF>
IGL_INLINE bool igl::is_vertex_manifold(
  const Eigen::MatrixBase<DerivedF>& F)
{
  Eigen::Array<bool,Eigen::Dynamic,1> B;
  return is_vertex_manifold(F,B);
}

#ifdef IGL_STATIC_LIBRARY
template bool igl::is_vertex_manifold<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(  Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template bool igl::is_vertex_manifold<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::is_vertex_manifold<Eigen::Matrix<int, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&);
#endif
