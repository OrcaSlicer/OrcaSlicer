// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "bbw.h"
#include "mosek_quadprog.h"
#include "../harmonic.h"
#include "../slice_into.h"
#include <Eigen/Sparse>
#include <iostream>
#include <cstdio>


template <
  typename DerivedV,
  typename DerivedEle,
  typename Derivedb,
  typename Derivedbc,
  typename DerivedW>
IGL_INLINE bool igl::mosek::bbw(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedEle> & Ele,
  const Eigen::PlainObjectBase<Derivedb> & b,
  const Eigen::PlainObjectBase<Derivedbc> & bc,
  igl::BBWData & data,
  igl::mosek::MosekData & mosek_data,
  Eigen::PlainObjectBase<DerivedW> & W
  )
{
  using namespace std;
  using namespace Eigen;
  assert(!data.partition_unity && "partition_unity not implemented yet");
  // number of domain vertices
  int n = V.rows();
  // number of handles
  int m = bc.cols();
  // Build biharmonic operator
  Eigen::SparseMatrix<typename DerivedV::Scalar> Q;
  harmonic(V,Ele,2,Q);
  W.derived().resize(n,m);
  // No linear terms
  VectorXd c = VectorXd::Zero(n);
  // No linear constraints
  SparseMatrix<typename DerivedW::Scalar> A(0,n);
  VectorXd uc(0,1),lc(0,1);
  // Upper and lower box constraints (Constant bounds)
  VectorXd ux = VectorXd::Ones(n);
  VectorXd lx = VectorXd::Zero(n);
  // Loop over handles
  for(int i = 0;i<m;i++)
  {
    if(data.verbosity >= 1)
    {
      cout<<"BBW: Computing weight for handle "<<i+1<<" out of "<<m<<
        "."<<endl;
    }
    VectorXd bci = bc.col(i);
    VectorXd Wi;
    // impose boundary conditions via bounds
    slice_into(bci,b,ux);
    slice_into(bci,b,lx);
    bool r = mosek_quadprog(Q,c,0,A,lc,uc,lx,ux,mosek_data,Wi);
    if(!r)
    {
      return false;
    }
    W.col(i) = Wi;
  }
#ifndef NDEBUG
    const double min_rowsum = W.rowwise().sum().array().abs().minCoeff();
    if(min_rowsum < 0.1)
    {
      cerr<<"bbw.cpp: Warning, minimum row sum is very low. Consider more "
        "active set iterations or enforcing partition of unity."<<endl;
    }
#endif

  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::mosek::bbw<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, igl::BBWData&, igl::mosek::MosekData&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif

