// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2023 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "isolines_intrinsic.h"
#include "edge_crossings.h"
#include "cat.h"
#include "unique_edge_map.h"
#ifndef NDEBUG
#  include "is_edge_manifold.h"
#  include "is_vertex_manifold.h"
#endif
#include <unordered_map>
#include <vector>

template <
  typename DerivedF,
  typename DerivedS,
  typename Derivedvals,
  typename DerivediB,
  typename DerivediFI,
  typename DerivediE,
  typename DerivedI>
void igl::isolines_intrinsic(
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedS> & S,
  const Eigen::MatrixBase<Derivedvals> & vals,
  Eigen::PlainObjectBase<DerivediB> & iB,
  Eigen::PlainObjectBase<DerivediFI> & iFI,
  Eigen::PlainObjectBase<DerivediE> & iE,
  Eigen::PlainObjectBase<DerivedI> & I)
{
  using Index = typename DerivedF::Scalar;
  Eigen::Matrix<Index,Eigen::Dynamic,Eigen::Dynamic> uE;
  Eigen::Vector<Index,Eigen::Dynamic> EMAP,uEC,uEE;

  {
    Eigen::Matrix<Index,Eigen::Dynamic,Eigen::Dynamic> E;
    igl::unique_edge_map(F,E,uE,EMAP,uEC,uEE);
  }

  {
    std::vector<DerivediB> viB(vals.size());
    std::vector<DerivediFI> viFI(vals.size());
    std::vector<DerivediE> viE(vals.size());
    std::vector<DerivedI> vI(vals.size());
    int num_vertices = 0;
    for(int j = 0;j<vals.size();j++)
    {
      isolines_intrinsic(F,S,uE,EMAP,uEC,uEE,vals(j),viB[j],viFI[j],viE[j]);
      viE[j].array() += num_vertices;
      num_vertices += viB[j].rows();
      vI[j] = DerivedI::Constant(viE[j].rows(),j);
    }
    igl::cat(1,viB,iB);
    igl::cat(1,viFI,iFI);
    igl::cat(1,viE,iE);
    igl::cat(1,vI,I);
  }
}

template <
  typename DerivedF,
  typename DerivedS,
  typename DeriveduE,
  typename DerivedEMAP,
  typename DeriveduEC,
  typename DeriveduEE,
  typename DerivediB,
  typename DerivediFI,
  typename DerivediE>
void igl::isolines_intrinsic(
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedS> & S,
  const Eigen::MatrixBase<DeriveduE> & uE,
  const Eigen::MatrixBase<DerivedEMAP> & EMAP,
  const Eigen::MatrixBase<DeriveduEC> & uEC,
  const Eigen::MatrixBase<DeriveduEE> & uEE,
  const typename DerivedS::Scalar val,
  Eigen::PlainObjectBase<DerivediB> & iB,
  Eigen::PlainObjectBase<DerivediFI> & iFI,
  Eigen::PlainObjectBase<DerivediE> & iE)
{
  using Scalar = typename DerivedS::Scalar;

  std::unordered_map<int,int> uE2I;
  Eigen::Matrix<Scalar,Eigen::Dynamic,1> T;
  igl::edge_crossings(uE,S,val,uE2I,T);

  iB.resize(uE2I.size(),F.cols());
  iFI.resize(uE2I.size());
  Eigen::VectorXi U(uE2I.size());
  for(auto & pair : uE2I)
  {
    const int u = pair.first;
    const int w = pair.second;
    // first face incident on uE(u,:)
    const int e = uEE(uEC(u));
    const int f = e % F.rows();
    const int k = e / F.rows();
    const bool flip = uE(u,0) != F(f,(k+1)%3);
    const double t = T(w);
    iB(w,k) = 0;
    iB(w,(k+1)%3) = flip?  t:1-t;
    iB(w,(k+2)%3) = flip?1-t:t;
    iFI(w) = f;
    U(w) = u;
  }
  
  
  // Vertex crossings
  std::unordered_map<int,int> V2I;
  {
    const auto add_vertex_crossing = [&iB,&iFI](const int k, const int i, const int j)
    {
      if(k >= iB.rows())
      {
        iB.conservativeResize(2*iB.rows()+1,Eigen::NoChange);
        iFI.conservativeResize(2*iFI.rows()+1,Eigen::NoChange);
      }
      iFI(k) = i;
      iB.row(k) << 0,0,0;
      iB(k,j) = 1;
    };
    int k = iB.rows();
    for(int i = 0;i<F.rows();i++)
    {
      for(int j = 0;j<3;j++)
      {
        const int v = F(i,j);
        if(S(v) == val)
        {
          if(V2I.find(v) == V2I.end())
          {
            V2I[v] = k;
            add_vertex_crossing(k++,i,j);
          }
        }
      }
    }
    iB.conservativeResize(k,Eigen::NoChange);
    iFI.conservativeResize(k,Eigen::NoChange);
  }

  iE.resize(uE2I.size(),2);
  const auto set_row = [&iE](const int k, const int i, const int j)
  {
    if(k >= iE.rows())
    {
      iE.conservativeResize(2*iE.rows()+1,Eigen::NoChange);
    }
    iE.row(k) << i,j;
  };
  {
    int r = 0;
    for(int f = 0;f < F.rows();f++)
    {
      // find first crossing edge
      int i;
      for(i = 0;i<3;i++)
      {
        if(uE2I.find(EMAP(f+F.rows()*i)) != uE2I.end())
        {
          break;
        }
      }
      int j;
      for(j = i+1;j<3;j++)
      {
        if(uE2I.find(EMAP(f+F.rows()*j)) != uE2I.end())
        {
          break;
        }
      }
      if(j<3)
      {
        // Connect two edge crossings.

        // other vertex
        const int k = 3-i-j;
        const int wi = uE2I[EMAP(f+F.rows()*i)];
        const int wj = uE2I[EMAP(f+F.rows()*j)];
        // flip orientation based on triangle gradient
        Scalar SFfk = S(F(f,k));
        bool flip = SFfk < val;
        flip = k%2? !flip:flip;
        if(flip)
        {
          set_row(r++,wi,wj);
        }else
        {
          set_row(r++,wj,wi);
        }
      }else if(i<3)
      {
        // The only valid vertex crossing is the opposite vertex
        const int v = F(f,i);
        // Is it a crossing?
        assert(V2I.find(v) != V2I.end());
        //if(V2I.find(v) != V2I.end())
        {
          const int wv = V2I[v];
          const int wi = uE2I[EMAP(f+F.rows()*i)];
          const bool flip = S(F(f,(i+1)%3)) > val;
          if(flip)
          {
            set_row(r++,wi,wv);
          }else
          {
            set_row(r++,wv,wi);
          }
        }
      }else
      {
        // Could have 2 vertex crossings. We're only interested if there're exactly two and if the other vertex is "above".
        int i = 0;
        for(i = 0;i<3;i++)
        {
          if(S(F(f,i)) == val)
          {
            break;
          }
        }
        int j;
        for(j = i+1;j<3;j++)
        {
          if(S(F(f,j)) == val)
          {
            break;
          }
        }
        if(j<3)
        {
          // check if the third is a crossing.
          const int k = 3-i-j;
          // Triangle is constant on the val. Skip.
          if(S(F(f,k)) == val){ continue; }
          // Is this a boundary edge?
          const int u = EMAP(f+F.rows()*k);
          const int count = uEC(u+1)-uEC(u);
          if( count == 1 || S(F(f,k)) > val)
          {
            const int wi = V2I[F(f,i)];
            const int wj = V2I[F(f,j)];
            bool flip = S(F(f,k)) < val;
            flip = k%2 ? !flip:flip;
            if(flip)
            {
              set_row(r++,wj,wi);
            }else
            {
              set_row(r++,wi,wj);
            }
          }
        }
      }
    }
    iE.conservativeResize(r,Eigen::NoChange);
  }

#ifdef IGL_ISOLINES_INTRINSIC_DEBUG
  if(igl::is_vertex_manifold(F) && igl::is_edge_manifold(F))
  {
    // Check that every vertex has one in one out
    Eigen::VectorXi in_count = Eigen::VectorXi::Zero(iB.rows());
    Eigen::VectorXi out_count = Eigen::VectorXi::Zero(iB.rows());
    for(int e = 0;e<iE.rows();e++)
    {
      const int i = iE(e,0);
      out_count(i)++;
      const int j = iE(e,1);
      in_count(j)++;
    }
    for(int i = 0;i<iB.rows();i++)
    {
      assert(in_count(i) <= 1);
      assert(out_count(i) <= 1);
    }
  }
#endif
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::isolines_intrinsic<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif

