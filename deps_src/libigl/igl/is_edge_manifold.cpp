// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "is_edge_manifold.h"
#include "oriented_facets.h"
#include "unique_simplices.h"
#include "unique_edge_map.h"

#include <algorithm>
#include <vector>

template <
  typename DerivedF,
  typename DerivedEMAP,
  typename DerivedBF,
  typename DerivedBE>
IGL_INLINE bool igl::is_edge_manifold(
  const Eigen::MatrixBase<DerivedF>& F,
  const typename DerivedF::Index ne,
  const Eigen::MatrixBase<DerivedEMAP>& EMAP,
  Eigen::PlainObjectBase<DerivedBF>& BF,
  Eigen::PlainObjectBase<DerivedBE>& BE)
{
  typedef typename DerivedF::Index Index;
  std::vector<Index> count(ne,0);
  for(Index e = 0;e<EMAP.rows();e++)
  {
    count[EMAP[e]]++;
  }
  const Index m = F.rows();
  BF.resize(m,3);
  BE.resize(ne,1);
  bool all = true;
  for(Index e = 0;e<EMAP.rows();e++)
  {
    const bool manifold = count[EMAP[e]] <= 2;
    all &= BF(e%m,e/m) = manifold;
    BE(EMAP[e]) = manifold;
  }
  return all;
}

template <
  typename DerivedF,
  typename DerivedBF,
  typename DerivedE,
  typename DerivedEMAP,
  typename DerivedBE>
IGL_INLINE bool igl::is_edge_manifold(
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedBF>& BF,
  Eigen::PlainObjectBase<DerivedE>& E,
  Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
  Eigen::PlainObjectBase<DerivedBE>& BE)
{
  using namespace Eigen;
  typedef Matrix<typename DerivedF::Scalar,Dynamic,2> MatrixXF2;
  MatrixXF2 allE;
  unique_edge_map(F,allE,E,EMAP);
  return is_edge_manifold(F,E.rows(),EMAP,BF,BE);
}

template <typename DerivedF>
IGL_INLINE bool igl::is_edge_manifold(
  const Eigen::MatrixBase<DerivedF>& F)
{
  Eigen::Array<bool,Eigen::Dynamic,Eigen::Dynamic> BF;
  Eigen::Array<bool,Eigen::Dynamic,1> BE;
  Eigen::MatrixXi E;
  Eigen::VectorXi EMAP;
  return is_edge_manifold(F,BF,E,EMAP,BE);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::is_edge_manifold<Eigen::Matrix<unsigned int, -1, -1, 1, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<unsigned int, -1, -1, 1, -1, -1> > const&);
template bool igl::is_edge_manifold<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
template bool igl::is_edge_manifold<Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&);
template bool igl::is_edge_manifold<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Array<bool, -1, -1, 0, -1, -1>, Eigen::Array<bool, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Array<bool, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Array<bool, -1, 1, 0, -1, 1> >&);
#endif
