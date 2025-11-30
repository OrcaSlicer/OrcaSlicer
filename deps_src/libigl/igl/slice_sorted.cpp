// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "slice_sorted.h"

#include <vector>

// TODO: Write a version that works for row-major sparse matrices as well.
template <typename TX, typename TY, typename DerivedR, typename DerivedC>
IGL_INLINE void igl::slice_sorted(const Eigen::SparseMatrix<TX> &X,
                                  const Eigen::DenseBase<DerivedR> &R,
                                  const Eigen::DenseBase<DerivedC> &C,
                                  Eigen::SparseMatrix<TY> &Y)
{
  int xm = X.rows();
  int xn = X.cols();
  int ym = R.size();
  int yn = C.size();

  // Special case when R or C is empty
  if (ym == 0 || yn == 0)
  {
    Y.resize(ym, yn);
    return;
  }

  assert(R.minCoeff() >= 0);
  assert(R.maxCoeff() < xm);
  assert(C.minCoeff() >= 0);
  assert(C.maxCoeff() < xn);

  // Multiplicity count for each row/col
  using RowIndexType = typename DerivedR::Scalar;
  using ColIndexType = typename DerivedC::Scalar;
  std::vector<RowIndexType> slicedRowStart(xm);
  std::vector<RowIndexType> rowRepeat(xm, 0);
  for (int i = 0; i < ym; ++i)
  {
    if (rowRepeat[R(i)] == 0)
    {
      slicedRowStart[R(i)] = i;
    }
    rowRepeat[R(i)]++;
  }
  std::vector<ColIndexType> columnRepeat(xn, 0);
  for (int i = 0; i < yn; i++)
  {
    columnRepeat[C(i)]++;
  }
  // Count number of nnz per outer row/col
  Eigen::VectorXi nnz(yn);
  for (int k = 0, c = 0; k < X.outerSize(); ++k)
  {
    int cnt = 0;
    for (typename Eigen::SparseMatrix<TX>::InnerIterator it(X, k); it; ++it)
    {
      cnt += rowRepeat[it.row()];
    }
    for (int i = 0; i < columnRepeat[k]; ++i, ++c)
    {
      nnz(c) = cnt;
    }
  }
  Y.resize(ym, yn);
  Y.reserve(nnz);
  // Insert values
  for (int k = 0, c = 0; k < X.outerSize(); ++k)
  {
    for (int i = 0; i < columnRepeat[k]; ++i, ++c)
    {
      for (typename Eigen::SparseMatrix<TX>::InnerIterator it(X, k); it; ++it)
      {
        for (int j = 0, r = slicedRowStart[it.row()]; j < rowRepeat[it.row()]; ++j, ++r)
        {
          Y.insert(r, c) = it.value();
        }
      }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
template void igl::slice_sorted<double, double, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::SparseMatrix<double, 0, int> const&, Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::SparseMatrix<double, 0, int>&);
#endif
