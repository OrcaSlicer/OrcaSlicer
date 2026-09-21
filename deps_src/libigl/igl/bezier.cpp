// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "bezier.h"
#include "PlainMatrix.h"
#include <cassert>

// Adapted from main.c accompanying
// An Algorithm for Automatically Fitting Digitized Curves
// by Philip J. Schneider
// from "Graphics Gems", Academic Press, 1990
template <typename DerivedV, typename DerivedP>
IGL_INLINE void igl::bezier(
  const Eigen::MatrixBase<DerivedV> & V,
  const typename DerivedV::Scalar t,
  Eigen::PlainObjectBase<DerivedP> & P)
{
  // working local copy
  PlainMatrix<DerivedV> Vtemp = V;
  int degree = Vtemp.rows()-1;
  /* Triangle computation	*/
  for (int i = 1; i <= degree; i++)
  {	
    for (int j = 0; j <= degree-i; j++) 
    {
      Vtemp.row(j) = ((1.0 - t) * Vtemp.row(j) + t * Vtemp.row(j+1)).eval();
    }
  }
  P = Vtemp.row(0);
}

template <typename DerivedV, typename DerivedT, typename DerivedP>
IGL_INLINE void igl::bezier(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedT> & T,
  Eigen::PlainObjectBase<DerivedP> & P)
{
  P.resize(T.size(),V.cols());
  for(int i = 0;i<T.size();i++)
  {
    Eigen::Matrix<typename DerivedV::Scalar,1,DerivedV::ColsAtCompileTime> Pi;
    bezier(V,T(i),Pi);
    P.row(i) = Pi;
  }
}

template <typename VMat, typename DerivedT, typename DerivedP>
IGL_INLINE void igl::bezier(
  const std::vector<VMat> & spline,
  const Eigen::MatrixBase<DerivedT> & T,
  Eigen::PlainObjectBase<DerivedP> & P)
{
  if(spline.size() == 0) return;
  const int m = T.rows();
  const int dim = spline[0].cols();
  P.resize(m*spline.size(),dim);
  for(int c = 0;c<spline.size();c++)
  {
    assert(dim == spline[c].cols() && "All curves must have same dimension");
    DerivedP Pc;
    bezier(spline[c],T,Pc);
    P.block(m*c,0,m,dim) = Pc;
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::bezier<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(std::vector<Eigen::Matrix<double, -1, -1, 0, -1, -1>, std::allocator<Eigen::Matrix<double, -1, -1, 0, -1, -1> > > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::bezier<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::bezier<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 1, -1, 1, 1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> >&);
#endif
