// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "heat_geodesics.h"
#include "grad.h"
#include "doublearea.h"
#include "cotmatrix.h"
#include "intrinsic_delaunay_cotmatrix.h"
#include "massmatrix.h"
#include "PlainVector.h"
#include "massmatrix_intrinsic.h"
#include "grad_intrinsic.h"
#include "boundary_facets.h"
#include "unique.h"
#include "avg_edge_length.h"
#include "PlainMatrix.h"


template < typename DerivedV, typename DerivedF, typename Scalar >
IGL_INLINE bool igl::heat_geodesics_precompute(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  HeatGeodesicsData<Scalar> & data)
{
  // default t value
  const Scalar h = avg_edge_length(V,F);
  const Scalar t = h*h;
  return heat_geodesics_precompute(V,F,t,data);
}

template < typename DerivedV, typename DerivedF, typename Scalar >
IGL_INLINE bool igl::heat_geodesics_precompute(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Scalar t,
  HeatGeodesicsData<Scalar> & data)
{
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,1> VectorXS;
  Eigen::SparseMatrix<Scalar> L,M;
  Eigen::Matrix<Scalar,Eigen::Dynamic,3> l_intrinsic;
  PlainMatrix<DerivedF> F_intrinsic;
  VectorXS dblA;
  if(data.use_intrinsic_delaunay)
  {
    igl::intrinsic_delaunay_cotmatrix(V,F,L,l_intrinsic,F_intrinsic);
    igl::massmatrix_intrinsic(l_intrinsic,F_intrinsic,MASSMATRIX_TYPE_DEFAULT,M);
    igl::doublearea(l_intrinsic,0,dblA);
    igl::grad_intrinsic(l_intrinsic,F_intrinsic,data.Grad);
  }else
  {
    igl::cotmatrix(V,F,L);
    igl::massmatrix(V,F,MASSMATRIX_TYPE_DEFAULT,M);
    igl::doublearea(V,F,dblA);
    igl::grad(V,F,data.Grad);
  }
  // div
  assert(F.cols() == 3 && "Only triangles are supported");
  // number of gradient components
  data.ng = data.Grad.rows() / F.rows();
  assert(data.ng == 3 || data.ng == 2);
  data.Div = -0.25*data.Grad.transpose()*dblA.colwise().replicate(data.ng).asDiagonal();

  Eigen::SparseMatrix<Scalar> Q = M - t*L;
  Eigen::MatrixXi O;
  igl::boundary_facets(F,O);
  igl::unique(O,data.b);
  {
    Eigen::SparseMatrix<Scalar> _;
    if(!igl::min_quad_with_fixed_precompute(
      Q,Eigen::VectorXi(),_,true,data.Neumann))
    {
      return false;
    }
    // Only need if there's a boundary
    if(data.b.size()>0)
    {
      if(!igl::min_quad_with_fixed_precompute(Q,data.b,_,true,data.Dirichlet))
      {
        return false;
      }
    }
    const Eigen::Matrix<Scalar,1,Eigen::Dynamic> M_diag_tr = M.diagonal().transpose();
    const Eigen::SparseMatrix<Scalar> Aeq = M_diag_tr.sparseView();
    L *= -0.5;
    if(!igl::min_quad_with_fixed_precompute(
      L,Eigen::VectorXi(),Aeq,true,data.Poisson))
    {
      return false;
    }
  }
  return true;
}

template < typename Scalar, typename Derivedgamma, typename DerivedD>
IGL_INLINE void igl::heat_geodesics_solve(
  const HeatGeodesicsData<Scalar> & data,
  const Eigen::MatrixBase<Derivedgamma> & gamma,
  Eigen::PlainObjectBase<DerivedD> & D)
{
  // number of mesh vertices
  const int n = data.Grad.cols();
  // Set up delta at gamma
  DerivedD u0 = DerivedD::Zero(n,1);
  for(int g = 0;g<gamma.size();g++)
  {
    u0(gamma(g)) = 1;
  }
  // Neumann solution
  DerivedD u;
  igl::min_quad_with_fixed_solve(
    data.Neumann,u0,DerivedD(),DerivedD(),u);
  if(data.b.size()>0)
  {
    // Average Dirichelt and Neumann solutions
    DerivedD uD;
    igl::min_quad_with_fixed_solve(
      data.Dirichlet,u0,DerivedD::Zero(data.b.size()).eval(),DerivedD(),uD);
    u += uD;
    u *= 0.5;
  }
  DerivedD grad_u = data.Grad*u;
  const int m = data.Grad.rows()/data.ng;
  for(int i = 0;i<m;i++)
  {
    // It is very important to use a stable norm calculation here. If the
    // triangle is far from a source, then the floating point values in the
    // gradient can be _very_ small (e.g., 1e-300). The standard/naive norm
    // calculation will suffer from underflow. Dividing by the max value is more
    // stable. (Eigen implements this as stableNorm or blueNorm).
    Scalar norm = 0;
    Scalar ma = 0;
    for(int d = 0;d<data.ng;d++) {ma = std::max(ma,std::fabs(grad_u(d*m+i)));}
    for(int d = 0;d<data.ng;d++)
    {
      const Scalar gui = grad_u(d*m+i) / ma;
      norm += gui*gui;
    }
    norm = ma*sqrt(norm);
    // These are probably over kill; ma==0 should be enough
    if(ma == 0 || norm == 0 || norm!=norm)
    {
      for(int d = 0;d<data.ng;d++) { grad_u(d*m+i) = 0; }
    }else
    {
      for(int d = 0;d<data.ng;d++) { grad_u(d*m+i) /= norm; }
    }
  }
  const DerivedD div_X = -data.Div*grad_u;
  const DerivedD Beq = (DerivedD(1,1)<<0).finished();
  igl::min_quad_with_fixed_solve(data.Poisson,(-div_X).eval(),DerivedD(),Beq,D);
  DerivedD Dgamma = D(gamma.derived());
  D.array() -= Dgamma.mean();
  if(D.mean() < 0)
  {
    D = -D;
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::heat_geodesics_solve<double, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(igl::HeatGeodesicsData<double> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template bool igl::heat_geodesics_precompute<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, double, igl::HeatGeodesicsData<double>&);
template bool igl::heat_geodesics_precompute<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::HeatGeodesicsData<double>&);
#endif
