// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "edge_topology.h"
#include "is_edge_manifold.h"
#include <algorithm>

template<typename DerivedV, typename DerivedF, typename DerivedE>
IGL_INLINE void igl::edge_topology(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedE>& EV,
  Eigen::PlainObjectBase<DerivedE>& FE,
  Eigen::PlainObjectBase<DerivedE>& EF)
{
  // Only needs to be edge-manifold
  if (V.rows() ==0 || F.rows()==0)
  {
    EV = Eigen::PlainObjectBase<DerivedE>::Constant(0,2,-1);
    FE = Eigen::PlainObjectBase<DerivedE>::Constant(0,3,-1);
    EF = Eigen::PlainObjectBase<DerivedE>::Constant(0,2,-1);
    return;
  }
  assert(igl::is_edge_manifold(F));
  std::vector<std::vector<typename DerivedE::Scalar> > ETT;
  for(int f=0;f<F.rows();++f)
    for (int i=0;i<3;++i)
    {
      // v1 v2 f vi
      int v1 = F(f,i);
      int v2 = F(f,(i+1)%3);
      if (v1 > v2) std::swap(v1,v2);
      std::vector<typename DerivedE::Scalar> r(4);
      r[0] = v1; r[1] = v2;
      r[2] = f;  r[3] = i;
      ETT.push_back(r);
    }
  std::sort(ETT.begin(),ETT.end());

  // count the number of edges (assume manifoldness)
  int En = 1; // the last is always counted
  for(int i=0;i<int(ETT.size())-1;++i)
    if (!((ETT[i][0] == ETT[i+1][0]) && (ETT[i][1] == ETT[i+1][1])))
      ++En;

  EV = DerivedE::Constant((int)(En),2,-1);
  FE = DerivedE::Constant((int)(F.rows()),3,-1);
  EF = DerivedE::Constant((int)(En),2,-1);
  En = 0;

  for(unsigned i=0;i<ETT.size();++i)
  {
    if (i == ETT.size()-1 ||
        !((ETT[i][0] == ETT[i+1][0]) && (ETT[i][1] == ETT[i+1][1]))
        )
    {
      // Border edge
      std::vector<typename DerivedE::Scalar>& r1 = ETT[i];
      EV(En,0)     = r1[0];
      EV(En,1)     = r1[1];
      EF(En,0)    = r1[2];
      FE(r1[2],r1[3]) = En;
    }
    else
    {
      std::vector<typename DerivedE::Scalar>& r1 = ETT[i];
      std::vector<typename DerivedE::Scalar>& r2 = ETT[i+1];
      EV(En,0)     = r1[0];
      EV(En,1)     = r1[1];
      EF(En,0)    = r1[2];
      EF(En,1)    = r2[2];
      FE(r1[2],r1[3]) = En;
      FE(r2[2],r2[3]) = En;
      ++i; // skip the next one
    }
    ++En;
  }

  // Sort the relation EF, accordingly to EV
  // the first one is the face on the left of the edge
  for(unsigned i=0; i<EF.rows(); ++i)
  {
    typename DerivedE::Scalar fid = EF(i,0);
    bool flip = true;
    // search for edge EV.row(i)
    for (unsigned j=0; j<3; ++j)
    {
      if ((F(fid,j) == EV(i,0)) && (F(fid,(j+1)%3) == EV(i,1)))
        flip = false;
    }

    if (flip)
    {
      int tmp = EF(i,0);
      EF(i,0) = EF(i,1);
      EF(i,1) = tmp;
    }
  }

}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::edge_topology<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&);
template void igl::edge_topology<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&);
#endif
