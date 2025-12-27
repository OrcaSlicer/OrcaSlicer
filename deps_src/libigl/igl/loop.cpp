// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "loop.h"

#include <igl/adjacency_list.h>
#include <igl/triangle_triangle_adjacency.h>
#include <igl/unique.h>

#include <vector>

template <
  typename DerivedF,
  typename SType,
  typename DerivedNF>
IGL_INLINE void igl::loop(
  const int n_verts,
  const Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::SparseMatrix<SType>& S,
  Eigen::PlainObjectBase<DerivedNF> & NF)
{
  typedef Eigen::SparseMatrix<SType> SparseMat;
  typedef Eigen::Triplet<SType> Triplet_t;
  
  //Ref. https://graphics.stanford.edu/~mdfisher/subdivision.html
  //Heavily borrowing from igl::upsample
  
  DerivedF FF, FFi;
  triangle_triangle_adjacency(F, FF, FFi);
  std::vector<std::vector<typename DerivedF::Scalar>> adjacencyList;
  adjacency_list(F, adjacencyList, true);
  
  //Compute the number and positions of the vertices to insert (on edges)
  Eigen::MatrixXi NI = Eigen::MatrixXi::Constant(FF.rows(), FF.cols(), -1);
  Eigen::MatrixXi NIdoubles = Eigen::MatrixXi::Zero(FF.rows(), FF.cols());
  Eigen::VectorXi vertIsOnBdry = Eigen::VectorXi::Zero(n_verts);
  int counter = 0;
  for(int i=0; i<FF.rows(); ++i)
  {
    for(int j=0; j<3; ++j)
    {
      if(NI(i,j) == -1)
      {
        NI(i,j) = counter;
        NIdoubles(i,j) = 0;
        if (FF(i,j) != -1) 
        {
          //If it is not a boundary
          NI(FF(i,j), FFi(i,j)) = counter;
          NIdoubles(i,j) = 1;
        } else 
        {
          //Mark boundary vertices for later
          vertIsOnBdry(F(i,j)) = 1;
          vertIsOnBdry(F(i,(j+1)%3)) = 1;
        }
        ++counter;
      }
    }
  }
  
  const int& n_odd = n_verts;
  const int& n_even = counter;
  const int n_newverts = n_odd + n_even;
  
  //Construct vertex positions
  std::vector<Triplet_t> tripletList;
  for(int i=0; i<n_odd; ++i) 
  {
    //Old vertices
    const std::vector<int>& localAdjList = adjacencyList[i];
    if(vertIsOnBdry(i)==1) 
    {
      //Boundary vertex
      tripletList.emplace_back(i, localAdjList.front(), 1./8.);
      tripletList.emplace_back(i, localAdjList.back(), 1./8.);
      tripletList.emplace_back(i, i, 3./4.);
    } else 
    {
      const int n = localAdjList.size();
      const SType dn = n;
      SType beta;
      if(n==3)
      {
        beta = 3./16.;
      } else
      {
        beta = 3./8./dn;
      }
      for(int j=0; j<n; ++j)
      {
        tripletList.emplace_back(i, localAdjList[j], beta);
      }
      tripletList.emplace_back(i, i, 1.-dn*beta);
    }
  }
  for(int i=0; i<FF.rows(); ++i) 
  {
    //New vertices
    for(int j=0; j<3; ++j) 
    {
      if(NIdoubles(i,j)==0) 
      {
        if(FF(i,j)==-1) 
        {
          //Boundary vertex
          tripletList.emplace_back(NI(i,j) + n_odd, F(i,j), 1./2.);
          tripletList.emplace_back(NI(i,j) + n_odd, F(i, (j+1)%3), 1./2.);
        } else 
        {
          tripletList.emplace_back(NI(i,j) + n_odd, F(i,j), 3./8.);
          tripletList.emplace_back(NI(i,j) + n_odd, F(i, (j+1)%3), 3./8.);
          tripletList.emplace_back(NI(i,j) + n_odd, F(i, (j+2)%3), 1./8.);
          tripletList.emplace_back(NI(i,j) + n_odd, F(FF(i,j), (FFi(i,j)+2)%3), 1./8.);
        }
      }
    }
  }
  S.resize(n_newverts, n_verts);
  S.setFromTriplets(tripletList.begin(), tripletList.end());
  
  // Build the new topology (Every face is replaced by four)
  NF.resize(F.rows()*4, 3);
  for(int i=0; i<F.rows();++i)
  {
    Eigen::VectorXi VI(6);
    VI << F(i,0), F(i,1), F(i,2), NI(i,0) + n_odd, NI(i,1) + n_odd, NI(i,2) + n_odd;
    
    Eigen::VectorXi f0(3), f1(3), f2(3), f3(3);
    f0 << VI(0), VI(3), VI(5);
    f1 << VI(1), VI(4), VI(3);
    f2 << VI(3), VI(4), VI(5);
    f3 << VI(4), VI(2), VI(5);
    
    NF.row((i*4)+0) = f0;
    NF.row((i*4)+1) = f1;
    NF.row((i*4)+2) = f2;
    NF.row((i*4)+3) = f3;
  }
}

template <
  typename DerivedV, 
  typename DerivedF,
  typename DerivedNV,
  typename DerivedNF>
IGL_INLINE void igl::loop(
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedNV>& NV,
  Eigen::PlainObjectBase<DerivedNF>& NF,
  const int number_of_subdivs)
{
  NV = V;
  NF = F;
  for(int i=0; i<number_of_subdivs; ++i) 
  {
    DerivedNF tempF = NF;
    Eigen::SparseMatrix<typename DerivedV::Scalar> S;
    loop(NV.rows(), tempF, S, NF);
    // This .eval is super important
    NV = (S*NV).eval();
  }
}

#ifdef IGL_STATIC_LIBRARY
template void igl::loop<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, int);
#endif
