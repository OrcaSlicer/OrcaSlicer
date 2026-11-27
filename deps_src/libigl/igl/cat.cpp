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

  // This is faster than using DynamicSparseMatrix or setFromTriplets
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

template <typename T, typename DerivedC>
IGL_INLINE void igl::cat(const int dim, const std::vector<T> & A, Eigen::PlainObjectBase<DerivedC> & C)
{
  assert(dim == 1 || dim == 2);
  using namespace Eigen;

  const int num_mat = A.size();
  if(num_mat == 0)
  {
    C.resize(0,0);
    return;
  }

  if(dim == 1)
  {
    const int A_cols = A[0].cols();

    int tot_rows = 0;
    for(const auto & m : A)
    {
      tot_rows += m.rows();
    }

    C.resize(tot_rows, A_cols);

    int cur_row = 0;
    for(int i = 0; i < num_mat; i++)
    {
      assert(A_cols == A[i].cols());
      C.block(cur_row,0,A[i].rows(),A_cols) = A[i];
      cur_row += A[i].rows();
    }
  }
  else if(dim == 2)
  {
    const int A_rows = A[0].rows();

    int tot_cols = 0;
    for(const auto & m : A)
    {
      tot_cols += m.cols();
    }

    C.resize(A_rows,tot_cols);

    int cur_col = 0;
    for(int i = 0; i < num_mat; i++)
    {
      assert(A_rows == A[i].rows());
      C.block(0,cur_col,A_rows,A[i].cols()) = A[i];
      cur_col += A[i].cols();
    }
  }
  else
  {
    fprintf(stderr,"cat.h: Error: Unsupported dimension %d\n",dim);
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::cat<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(int, std::vector<Eigen::Matrix<int, -1, 1, 0, -1, 1>, std::allocator<Eigen::Matrix<int, -1, 1, 0, -1, 1> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
// generated by autoexplicit.sh
template void igl::cat<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<double, -1, -1, 0, -1, -1>, std::allocator<Eigen::Matrix<double, -1, -1, 0, -1, -1> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template void igl::cat<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<float, -1, -1, 0, -1, -1>, std::allocator<Eigen::Matrix<float, -1, -1, 0, -1, -1> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&);
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
template void igl::cat<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(int, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::Matrix<int, -1, 1, 0, -1, 1>&);
template void igl::cat<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<int, -1, -1, 0, -1, -1>, std::allocator<Eigen::Matrix<int, -1, -1, 0, -1, -1> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cat<Eigen::Matrix<int, 1, 4, 1, 1, 4>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<int, 1, 4, 1, 1, 4>, std::allocator<Eigen::Matrix<int, 1, 4, 1, 1, 4> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cat<Eigen::Matrix<int, 1, 15, 1, 1, 15>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<int, 1, 15, 1, 1, 15>, std::allocator<Eigen::Matrix<int, 1, 15, 1, 1, 15> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cat<Eigen::Matrix<int, 1, 2, 1, 1, 2>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<int, 1, 2, 1, 1, 2>, std::allocator<Eigen::Matrix<int, 1, 2, 1, 1, 2> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cat<Eigen::Matrix<int, 1, 27, 1, 1, 27>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<int, 1, 27, 1, 1, 27>, std::allocator<Eigen::Matrix<int, 1, 27, 1, 1, 27> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cat<Eigen::Matrix<int, 1, 3, 1, 1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<int, 1, 3, 1, 1, 3>, std::allocator<Eigen::Matrix<int, 1, 3, 1, 1, 3> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cat<Eigen::Matrix<int, 3, 1, 0, 3, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<int, 3, 1, 0, 3, 1>, std::allocator<Eigen::Matrix<int, 3, 1, 0, 3, 1> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cat<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<double, 1, 3, 1, 1, 3>, std::allocator<Eigen::Matrix<double, 1, 3, 1, 1, 3> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::cat<Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<double, 3, 1, 0, 3, 1>, std::allocator<Eigen::Matrix<double, 3, 1, 0, 3, 1> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::cat<Eigen::Matrix<int, 1, -1, 1, 1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<int, 1, -1, 1, 1, -1>, std::allocator<Eigen::Matrix<int, 1, -1, 1, 1, -1> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cat<Eigen::Matrix<double, 1, 2, 1, 1, 2>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(int, std::vector<Eigen::Matrix<double, 1, 2, 1, 1, 2>, std::allocator<Eigen::Matrix<double, 1, 2, 1, 1, 2> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
