// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "facet_components.h"
#include "triangle_triangle_adjacency.h"
#include "facet_adjacency_matrix.h"
#include "connected_components.h"
#include <vector>
#include <queue>

template <typename DerivedF, typename DerivedC>
IGL_INLINE int igl::facet_components(
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedC> & C)
{
  typedef typename DerivedF::Scalar Index;
  Eigen::SparseMatrix<Index> A;
  igl::facet_adjacency_matrix(F,A);
  Eigen::Matrix<Index,Eigen::Dynamic,1> counts;
  C = DerivedC::Zero(1,1);
  return connected_components(A,C,counts);
}

template <
  typename TTIndex,
  typename DerivedC,
  typename Derivedcounts>
IGL_INLINE void igl::facet_components(
  const std::vector<std::vector<std::vector<TTIndex > > > & TT,
  Eigen::PlainObjectBase<DerivedC> & C,
  Eigen::PlainObjectBase<Derivedcounts> & counts)
{
  using namespace std;
  typedef TTIndex Index;
  const Index m = TT.size();
  C.resize(m,1);
  vector<bool> seen(m,false);
  Index id = 0;
  vector<Index> vcounts;
  for(Index g = 0;g<m;g++)
  {
    if(seen[g])
    {
      continue;
    }
    vcounts.push_back(0);
    queue<Index> Q;
    Q.push(g);
    while(!Q.empty())
    {
      const Index f = Q.front();
      Q.pop();
      if(seen[f])
      {
        continue;
      }
      seen[f] = true;
      vcounts[id]++;
      C(f,0) = id;
      // Face f's neighbor lists opposite opposite each corner
      for(const auto & c : TT[f])
      {
        // Each neighbor
        for(const auto & n : c)
        {
          if(!seen[n])
          {
            Q.push(n);
          }
        }
      }
    }
    id++;
  }
  assert((size_t) id == vcounts.size());
  const size_t ncc = vcounts.size();
  assert((size_t)C.maxCoeff()+1 == ncc);
  counts.resize(ncc,1);
  for(size_t i = 0;i<ncc;i++)
  {
    counts(i) = vcounts[i];
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::facet_components<long, Eigen::Matrix<long, -1, 1, 0, -1, 1>, Eigen::Matrix<long, -1, 1, 0, -1, 1> >(std::vector<std::vector<std::vector<long, std::allocator<long> >, std::allocator<std::vector<long, std::allocator<long> > > >, std::allocator<std::vector<std::vector<long, std::allocator<long> >, std::allocator<std::vector<long, std::allocator<long> > > > > > const&, Eigen::PlainObjectBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&);
template void igl::facet_components<int, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(std::vector<std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >, std::allocator<std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template int igl::facet_components<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#ifdef WIN32
template void igl::facet_components<__int64,class Eigen::Matrix<__int64,-1,1,0,-1,1>,class Eigen::Matrix<__int64,-1,1,0,-1,1> >(class std::vector<class std::vector<class std::vector<__int64,class std::allocator<__int64> >,class std::allocator<class std::vector<__int64,class std::allocator<__int64> > > >,class std::allocator<class std::vector<class std::vector<__int64,class std::allocator<__int64> >,class std::allocator<class std::vector<__int64,class std::allocator<__int64> > > > > > const &,class Eigen::PlainObjectBase<class Eigen::Matrix<__int64,-1,1,0,-1,1> > &,class Eigen::PlainObjectBase<class Eigen::Matrix<__int64,-1,1,0,-1,1> > &);
#endif
#endif
