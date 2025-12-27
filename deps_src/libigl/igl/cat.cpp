// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "cat.h"

#include <cstdio>

// Bug in unsupported/Eigen/SparseExtra needs iostream first
#include <iostream>
#include <unsupported/Eigen/SparseExtra>


// Sparse matrices need to be handled carefully. Because C++ does not 
// Template:
//   Scalar  sparse matrix scalar type, e.g. double
template <typename Scalar>
IGL_INLINE void igl::cat(
    const int dim, 
    const Eigen::SparseMatrix<Scalar> & A, 
    const Eigen::SparseMatrix<Scalar> & B, 
    Eigen::SparseMatrix<Scalar> & C)
{

  assert(dim == 1 || dim == 2);
  using namespace Eigen;
  // Special case if B or A is empty
  if(A.size() == 0)
  {
    C = B;
    return;
  }
  if(B.size() == 0)
  {
    C = A;
    return;
  }

#if false
  // This **must** be DynamicSparseMatrix, otherwise this implementation is
  // insanely slow
  DynamicSparseMatrix<Scalar, RowMajor> dyn_C;
  if(dim == 1)
  {
    assert(A.cols() == B.cols());
    dyn_C.resize(A.rows()+B.rows(),A.cols());
  }else if(dim == 2)
  {
    assert(A.rows() == B.rows());
    dyn_C.resize(A.rows(),A.cols()+B.cols());
  }else
  {
    fprintf(stderr,"cat.h: Error: Unsupported dimension %d\n",dim);
  }

  dyn_C.reserve(A.nonZeros()+B.nonZeros());

  // Iterate over outside of A
  for(int k=0; k<A.outerSize(); ++k)
  {
    // Iterate over inside
    for(typename SparseMatrix<Scalar>::InnerIterator it (A,k); it; ++it)
    {
      dyn_C.coeffRef(it.row(),it.col()) += it.value();
    }
  }

  // Iterate over outside of B
  for(int k=0; k<B.outerSize(); ++k)
  {
    // Iterate over inside
    for(typename SparseMatrix<Scalar>::InnerIterator it (B,k); it; ++it)
    {
      int r = (dim == 1 ? A.rows()+it.row() : it.row());
      int c = (dim == 2 ? A.cols()+it.col() : it.col());
      dyn_C.coeffRef(r,c) += it.value();
    }
  }

  C = SparseMatrix<Scalar>(dyn_C);
#elif false
  std::vector<Triplet<Scalar> > CIJV;
  CIJV.reserve(A.nonZeros() + B.nonZeros());
  {
    // Iterate over outside of A
    for(int k=0; k<A.outerSize(); ++k)
    {
      // Iterate over inside
      for(typename SparseMatrix<Scalar>::InnerIterator it (A,k); it; ++it)
      {
        CIJV.emplace_back(it.row(),it.col(),it.value());
      }
    }
    // Iterate over outside of B
    for(int k=0; k<B.outerSize(); ++k)
    {
      // Iterate over inside
      for(typename SparseMatrix<Scalar>::InnerIterator it (B,k); it; ++it)
      {
        int r = (dim == 1 ? A.rows()+it.row() : it.row());
        int c = (dim == 2 ? A.cols()+it.col() : it.col());
        CIJV.emplace_back(r,c,it.value());
      }
    }

  }

  C = SparseMatrix<Scalar>( 
      dim == 1 ? A.rows()+B.rows() : A.rows(),
      dim == 1 ? A.cols()          : A.cols()+B.cols());
  C.reserve(A.nonZeros() + B.nonZeros());
  C.setFromTriplets(CIJV.begin(),CIJV.end());
#else
  C = SparseMatrix<Scalar>( 
      dim == 1 ? A.rows()+B.rows() : A.rows(),
      dim == 1 ? A.cols()          : A.cols()+B.cols());
  Eigen::VectorXi per_col = Eigen::VectorXi::Zero(C.cols());
  if(dim == 1)
  {
    assert(A.outerSize() == B.outerSize());
    for(int k = 0;k<A.outerSize();++k)
    {
      for(typename SparseMatrix<Scalar>::InnerIterator it (A,k); it; ++it)
      {
        per_col(k)++;
      }
      for(typename SparseMatrix<Scalar>::InnerIterator it (B,k); it; ++it)
      {
        per_col(k)++;
      }
    }
  }else
  {
    for(int k = 0;k<A.outerSize();++k)
    {
      for(typename SparseMatrix<Scalar>::InnerIterator it (A,k); it; ++it)
      {
        per_col(k)++;
      }
    }
    for(int k = 0;k<B.outerSize();++k)
    {
      for(typename SparseMatrix<Scalar>::InnerIterator it (B,k); it; ++it)
      {
        per_col(A.cols() + k)++;
      }
    }
  }
  C.reserve(per_col);
  if(dim == 1)
  {
    for(int k = 0;k<A.outerSize();++k)
    {
      for(typename SparseMatrix<Scalar>::InnerIterator it (A,k); it; ++it)
      {
        C.insert(it.row(),k) = it.value();
      }
      for(typename SparseMatrix<Scalar>::InnerIterator it (B,k); it; ++it)
      {
        C.insert(A.rows()+it.row(),k) = it.value();
      }
    }
  }else
  {
    for(int k = 0;k<A.outerSize();++k)
    {
      for(typename SparseMatrix<Scalar>::InnerIterator it (A,k); it; ++it)
      {
        C.insert(it.row(),k) = it.value();
      }
    }
    for(int k = 0;k<B.outerSize();++k)
    {
      for(typename SparseMatrix<Scalar>::InnerIterator it (B,k); it; ++it)
      {
        C.insert(it.row(),A.cols()+k) = it.value();
      }
    }
  }
  C.makeCompressed();

#endif

}

template <typename Derived, class MatC>
IGL_INLINE void igl::cat(
  const int dim,
  const Eigen::MatrixBase<Derived> & A, 
  const Eigen::MatrixBase<Derived> & B,
  MatC & C)
{
  assert(dim == 1 || dim == 2);
  // Special case if B or A is empty
  if(A.size() == 0)
  {
    C = B;
    return;
  }
  if(B.size() == 0)
  {
    C = A;
    return;
  }

  if(dim == 1)
  {
    assert(A.cols() == B.cols());
    C.resize(A.rows()+B.rows(),A.cols());
    C << A,B;
  }else if(dim == 2)
  {
    assert(A.rows() == B.rows());
    C.resize(A.rows(),A.cols()+B.cols());
    C << A,B;
  }else
  {
    fprintf(stderr,"cat.h: Error: Unsupported dimension %d\n",dim);
  }
}

template <class Mat>
IGL_INLINE Mat igl::cat(const int dim, const Mat & A, const Mat & B)
{
  assert(dim == 1 || dim == 2);
  Mat C;
  igl::cat(dim,A,B,C);
  return C;
}

template <class Mat>
IGL_INLINE void igl::cat(const std::vector<std::vector< Mat > > & A, Mat & C)
{
  using namespace std;
  // Start with empty matrix
  C.resize(0,0);
  for(const auto & row_vec : A)
  {
    // Concatenate each row horizontally
    // Start with empty matrix
    Mat row(0,0);
    for(const auto & element : row_vec)
    {
      row = cat(2,row,element);
    }
    // Concatenate rows vertically
    C = cat(1,C,row);
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template Eigen::Matrix<double, -1, -1, 0, -1, -1> igl::cat<Eigen::Matrix<double, -1, -1, 0, -1, -1> >(int, Eigen::Matrix<double, -1, -1, 0, -1, -1> const&, Eigen::Matrix<double, -1, -1, 0, -1, -1> const&);
// generated by autoexplicit.sh
template Eigen::SparseMatrix<double, 0, int> igl::cat<Eigen::SparseMatrix<double, 0, int> >(int, Eigen::SparseMatrix<double, 0, int> const&, Eigen::SparseMatrix<double, 0, int> const&);
// generated by autoexplicit.sh
template Eigen::Matrix<int, -1, -1, 0, -1, -1> igl::cat<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, Eigen::Matrix<int, -1, -1, 0, -1, -1> const&, Eigen::Matrix<int, -1, -1, 0, -1, -1> const&);
template void igl::cat<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(int, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::Matrix<double, -1, 1, 0, -1, 1>&);
template Eigen::Matrix<int, -1, 1, 0, -1, 1> igl::cat<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(int, Eigen::Matrix<int, -1, 1, 0, -1, 1> const&, Eigen::Matrix<int, -1, 1, 0, -1, 1> const&);
template Eigen::Matrix<double, -1, 1, 0, -1, 1> igl::cat<Eigen::Matrix<double, -1, 1, 0, -1, 1> >(int, Eigen::Matrix<double, -1, 1, 0, -1, 1> const&, Eigen::Matrix<double, -1, 1, 0, -1, 1> const&);
template void igl::cat<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(int, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>&);
template void igl::cat<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1>&);
#endif
