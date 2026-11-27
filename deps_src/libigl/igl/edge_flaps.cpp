// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "edge_flaps.h"
#include "unique_edge_map.h"
#include <vector>
#include <cassert>

template <
  typename DerivedF,
  typename DeriveduE,
  typename DerivedEMAP,
  typename DerivedEF,
  typename DerivedEI>
IGL_INLINE void igl::edge_flaps(
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DeriveduE> & uE,
  const Eigen::MatrixBase<DerivedEMAP> & EMAP,
  Eigen::PlainObjectBase<DerivedEF> & EF,
  Eigen::PlainObjectBase<DerivedEI> & EI)
{
  // Initialize to boundary value
  EF.setConstant(uE.rows(),2,-1);
  EI.setConstant(uE.rows(),2,-1);
  // loop over all faces
  for(int f = 0;f<F.rows();f++)
  {
    // loop over edges across from corners
    for(int v = 0;v<3;v++)
    {
      // get edge id
      const int e = EMAP(v*F.rows()+f);
      // See if this is left or right flap w.r.t. edge orientation
      if( F(f,(v+1)%3) == uE(e,0) && F(f,(v+2)%3) == uE(e,1))
      {
        EF(e,0) = f;
        EI(e,0) = v;
      }else
      {
        assert(F(f,(v+1)%3) == uE(e,1) && F(f,(v+2)%3) == uE(e,0));
        EF(e,1) = f;
        EI(e,1) = v;
      }
    }
  }
}

template <
  typename DerivedF,
  typename DeriveduE,
  typename DerivedEMAP,
  typename DerivedEF,
  typename DerivedEI>
IGL_INLINE void igl::edge_flaps(
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DeriveduE> & uE,
  Eigen::PlainObjectBase<DerivedEMAP> & EMAP,
  Eigen::PlainObjectBase<DerivedEF> & EF,
  Eigen::PlainObjectBase<DerivedEI> & EI)
{
  Eigen::MatrixXi allE;
  igl::unique_edge_map(F,allE,uE,EMAP);
  // Const-ify to call overload
  const auto & cuE = uE;
  const auto & cEMAP = EMAP;
  return edge_flaps(F,cuE,cEMAP,EF,EI);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template specialization
template void igl::edge_flaps<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&);
template void igl::edge_flaps<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&);
#endif
