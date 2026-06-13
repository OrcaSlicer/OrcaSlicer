// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "point_solid_signed_squared_distance.h"
#include "points_inside_component.h"
#include "point_mesh_squared_distance.h"
#include "../../get_seconds.h"
#include "../../list_to_matrix.h"
#include "../../find.h"
#include "../../placeholders.h"
#include "../../parallel_for.h"
#include "../../PlainMatrix.h"
#include <vector>
#include <Eigen/Core>

template <
  typename DerivedQ,
  typename DerivedVB,
  typename DerivedFB,
  typename DerivedD>
IGL_INLINE void igl::copyleft::cgal::point_solid_signed_squared_distance(
  const Eigen::MatrixBase<DerivedQ> & Q,
  const Eigen::MatrixBase<DerivedVB> & VB,
  const Eigen::MatrixBase<DerivedFB> & FB,
  Eigen::PlainObjectBase<DerivedD> & D)
{
  // compute unsigned distances
  Eigen::VectorXi I;
  PlainMatrix<DerivedVB,Eigen::Dynamic> C;
  point_mesh_squared_distance<CGAL::Epeck>(Q,VB,FB,D,I,C);
  // Collect queries that have non-zero distance
  Eigen::Array<bool,Eigen::Dynamic,1> NZ = D.array()!=0;
  // Compute sign for non-zero distance queries
  PlainMatrix<DerivedQ,Eigen::Dynamic> QNZ = Q(igl::find(NZ),igl::placeholders::all);
  Eigen::Array<bool,Eigen::Dynamic,1> DNZ;
  igl::copyleft::cgal::points_inside_component(VB,FB,QNZ,DNZ);
  // Apply sign to distances
  DerivedD S = DerivedD::Zero(Q.rows(),1);
  {
    int k = 0;
    for(int q = 0;q<Q.rows();q++)
    {
      if(NZ(q))
      {
        D(q) *= DNZ(k++) ? -1. : 1.;
      }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::copyleft::cgal::point_solid_signed_squared_distance<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<CGAL::Epeck::FT, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 1, 0, -1, 1> >&);
#endif
