// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "find.h"

#include "verbose.h"
#include <iostream>
  
template <
  typename T, 
  typename DerivedI, 
  typename DerivedJ,
  typename DerivedV>
IGL_INLINE void igl::find(
  const Eigen::SparseMatrix<T>& X,
  Eigen::DenseBase<DerivedI> & I,
  Eigen::DenseBase<DerivedJ> & J,
  Eigen::DenseBase<DerivedV> & V)
{
  // Resize outputs to fit nonzeros
  I.derived().resize(X.nonZeros(),1);
  J.derived().resize(X.nonZeros(),1);
  V.derived().resize(X.nonZeros(),1);

  int i = 0;
  // Iterate over outside
  for(int k=0; k<X.outerSize(); ++k)
  {
    // Iterate over inside
    for(typename Eigen::SparseMatrix<T>::InnerIterator it (X,k); it; ++it)
    {
      V(i) = it.value();
      I(i) = it.row();
      J(i) = it.col();
      i++;
    }
  }
}

template <
  typename DerivedX,
  typename DerivedI, 
  typename DerivedJ,
  typename DerivedV>
IGL_INLINE void igl::find(
  const Eigen::DenseBase<DerivedX>& X,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedJ> & J,
  Eigen::PlainObjectBase<DerivedV> & V)
{
  const int nnz = X.count();
  I.resize(nnz,1);
  J.resize(nnz,1);
  V.resize(nnz,1);
  {
    int k = 0;
    for(int j = 0;j<X.cols();j++)
    {
      for(int i = 0;i<X.rows();i++)
      {
        if(X(i,j))
        {
          I(k) = i;
          J(k) = j;
          V(k) = X(i,j);
          k++;
        }
      }
    }
  }
}

template <
  typename DerivedX,
  typename DerivedI>
IGL_INLINE void igl::find(
  const Eigen::DenseBase<DerivedX>& X,
  Eigen::PlainObjectBase<DerivedI> & I)
{
  const int nnz = X.count();
  I.resize(nnz,1);
  {
    int k = 0;
    for(int j = 0;j<X.cols();j++)
    {
      for(int i = 0;i<X.rows();i++)
      {
        if(X(i,j))
        {
          I(k) = i+X.rows()*j;
          k++;
        }
      }
    }
  }
}
  
template <typename T>
IGL_INLINE void igl::find(
  const Eigen::SparseVector<T>& X,
  Eigen::Matrix<int,Eigen::Dynamic,1> & I,
  Eigen::Matrix<T,Eigen::Dynamic,1> & V)
{
  // Resize outputs to fit nonzeros
  I.resize(X.nonZeros());
  V.resize(X.nonZeros());

  int i = 0;
  // loop over non-zeros
  for(typename Eigen::SparseVector<T>::InnerIterator it(X); it; ++it)
  {
    I(i) = it.index();
    V(i) = it.value();
    i++;
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation

template void igl::find<bool, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Array<bool, -1, 1, 0, -1, 1> >(Eigen::SparseMatrix<bool, 0, int> const&, Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::DenseBase<Eigen::Array<bool, -1, 1, 0, -1, 1> >&);
template void igl::find<Eigen::Array<bool, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Array<bool, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::find<double, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::SparseMatrix<double, 0, int> const&, Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::DenseBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::find<double, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::SparseMatrix<double, 0, int> const&, Eigen::DenseBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::DenseBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::DenseBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::find<double, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::SparseMatrix<double, 0, int> const&, Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::DenseBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::find<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::find<double, Eigen::Matrix<long, -1, 1, 0, -1, 1>, Eigen::Matrix<long, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::SparseMatrix<double, 0, int> const&, Eigen::DenseBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&, Eigen::DenseBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&, Eigen::DenseBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
#if EIGEN_VERSION_AT_LEAST(3,3,0)
#else
template void igl::find<Eigen::CwiseBinaryOp<Eigen::internal::scalar_cmp_op<long, (Eigen::internal::ComparisonName)0>, Eigen::PartialReduxExpr<Eigen::Array<bool, -1, 3, 0, -1, 3>, Eigen::internal::member_count<long>, 1> const, Eigen::CwiseNullaryOp<Eigen::internal::scalar_constant_op<long>, Eigen::Array<long, -1, 1, 0, -1, 1> > const>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::CwiseBinaryOp<Eigen::internal::scalar_cmp_op<long, (Eigen::internal::ComparisonName)0>, Eigen::PartialReduxExpr<Eigen::Array<bool, -1, 3, 0, -1, 3>, Eigen::internal::member_count<long>, 1> const, Eigen::CwiseNullaryOp<Eigen::internal::scalar_constant_op<long>, Eigen::Array<long, -1, 1, 0, -1, 1> > const> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::find<Eigen::CwiseBinaryOp<Eigen::internal::scalar_cmp_op<int, (Eigen::internal::ComparisonName)0>, Eigen::Array<int, -1, 1, 0, -1, 1> const, Eigen::CwiseNullaryOp<Eigen::internal::scalar_constant_op<int>, Eigen::Array<int, -1, 1, 0, -1, 1> > const>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::CwiseBinaryOp<Eigen::internal::scalar_cmp_op<int, (Eigen::internal::ComparisonName)0>, Eigen::Array<int, -1, 1, 0, -1, 1> const, Eigen::CwiseNullaryOp<Eigen::internal::scalar_constant_op<int>, Eigen::Array<int, -1, 1, 0, -1, 1> > const> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif

#endif
