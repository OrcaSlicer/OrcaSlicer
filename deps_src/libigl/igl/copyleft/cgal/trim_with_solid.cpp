// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "trim_with_solid.h"
#include "assign.h"
#include "intersect_other.h"
#include "point_solid_signed_squared_distance.h"

#include "../../extract_manifold_patches.h"
#include "../../list_to_matrix.h"
#include "../../remove_unreferenced.h"
#include "../../slice_mask.h"

#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

#include <vector>

template <
  typename DerivedVA,
  typename DerivedFA,
  typename DerivedVB,
  typename DerivedFB,
  typename DerivedV,
  typename DerivedF,
  typename DerivedD,
  typename DerivedJ>
IGL_INLINE void igl::copyleft::cgal::trim_with_solid(
  const Eigen::PlainObjectBase<DerivedVA> & VA,
  const Eigen::PlainObjectBase<DerivedFA> & FA,
  const Eigen::PlainObjectBase<DerivedVB> & VB,
  const Eigen::PlainObjectBase<DerivedFB> & FB,
  Eigen::PlainObjectBase<DerivedV> & Vd,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedD> & D,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  // resolve intersections using exact representation
  typedef Eigen::Matrix<CGAL::Epeck::FT,Eigen::Dynamic,3> MatrixX3E;
  typedef Eigen::Matrix<CGAL::Epeck::FT,Eigen::Dynamic,1> VectorXE;
  typedef Eigen::Matrix<CGAL::Epeck::FT,1,3> RowVector3E;
  MatrixX3E V;
  Eigen::MatrixXi _1;
  Eigen::VectorXi _2;
  // Intersect A and B meshes and stitch together new faces
  igl::copyleft::cgal::intersect_other(
    VA,FA,VB,FB,{false,false,true},_1,V,F,J,_2);
  // Partition result into manifold patches
  Eigen::VectorXi P;
  const size_t num_patches = igl::extract_manifold_patches(F,P);
  // only keep faces from A
  Eigen::Matrix<bool,Eigen::Dynamic,1> A = J.array()< FA.rows();
  igl::slice_mask(Eigen::MatrixXi(F),A,1,F);
  igl::slice_mask(Eigen::VectorXi(P),A,1,P);
  igl::slice_mask(Eigen::VectorXi(J),A,1,J);
  // Aggregate representative query points for each patch
  std::vector<bool> flag(num_patches);
  std::vector<std::vector<CGAL::Epeck::FT> > vQ;
  Eigen::VectorXi P2Q(num_patches);
  for(int f = 0;f<P.rows();f++)
  {
    const auto p = P(f);
    // if not yet processed this patch
    if(!flag[p])
    {
      P2Q(p) = vQ.size();
      std::vector<CGAL::Epeck::FT> q = {
        (V(F(f,0),0)+ V(F(f,1),0)+ V(F(f,2),0))/3.,
        (V(F(f,0),1)+ V(F(f,1),1)+ V(F(f,2),1))/3.,
        (V(F(f,0),2)+ V(F(f,1),2)+ V(F(f,2),2))/3.};
      vQ.emplace_back(q);
      flag[p] = true;
    }
  }
  MatrixX3E Q;
  igl::list_to_matrix(vQ,Q);
  VectorXE SP;
  point_solid_signed_squared_distance(Q,VB,FB,SP);
  Eigen::Matrix<bool,Eigen::Dynamic,1> DP = SP.array()>0;
  // distribute flag to all faces
  D.resize(F.rows());
  for(int f = 0;f<F.rows();f++)
  {
    D(f) = DP(P2Q(P(f)));
  }
  Eigen::VectorXi _;
  igl::remove_unreferenced(MatrixX3E(V),DerivedF(F),V,F,_);
  assign(V,Vd);
}


#ifdef IGL_STATIC_LIBRARY
template void igl::copyleft::cgal::trim_with_solid<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1,
  -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0,
  -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>
  >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int,
  -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&,
  Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1,
  -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&,
  Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1,
  1> >&);
#endif
