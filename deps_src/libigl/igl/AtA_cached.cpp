// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2017 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "AtA_cached.h"

#include <iostream>
#include <vector>
#include <utility>

template <typename Scalar>
IGL_INLINE void igl::AtA_cached_precompute(
    const Eigen::SparseMatrix<Scalar>& A,
    igl::AtA_cached_data& data,
    Eigen::SparseMatrix<Scalar>& AtA)
{
  // 1 Compute At (this could be avoided, but performance-wise it will not make a difference)
  std::vector<std::vector<int> > Col_RowPtr;
  std::vector<std::vector<int> > Col_IndexPtr;

  Col_RowPtr.resize(A.cols());
  Col_IndexPtr.resize(A.cols());

  for (unsigned k=0; k<A.outerSize(); ++k)
  {
    unsigned outer_index = *(A.outerIndexPtr()+k);
    unsigned next_outer_index = (k+1 == A.outerSize()) ? A.nonZeros() : *(A.outerIndexPtr()+k+1); 
    
    for (unsigned l=outer_index; l<next_outer_index; ++l)
    {
      int col = k;
      int row = *(A.innerIndexPtr()+l);
      int value_index = l;
      assert(col < A.cols());
      assert(col >= 0);
      assert(row < A.rows());
      assert(row >= 0);
      assert(value_index >= 0);
      assert(value_index < A.nonZeros());

      Col_RowPtr[col].push_back(row);
      Col_IndexPtr[col].push_back(value_index);
    }
  }

  Eigen::SparseMatrix<Scalar> At = A.transpose();
  At.makeCompressed();
  AtA = At * A;
  AtA.makeCompressed();

  assert(AtA.isCompressed());

  // If weights are not provided, use 1
  if (data.W.size() == 0)
    data.W = Eigen::VectorXd::Ones(A.rows());
  assert(data.W.size() == A.rows());

  data.I_outer.reserve(AtA.outerSize());
  data.I_row.reserve(2*AtA.nonZeros());
  data.I_col.reserve(2*AtA.nonZeros());
  data.I_w.reserve(2*AtA.nonZeros());

  // 2 Construct the rules
  for (unsigned k=0; k<AtA.outerSize(); ++k)
  {
    unsigned outer_index = *(AtA.outerIndexPtr()+k);
    unsigned next_outer_index = (k+1 == AtA.outerSize()) ? AtA.nonZeros() : *(AtA.outerIndexPtr()+k+1); 
    
    for (unsigned l=outer_index; l<next_outer_index; ++l)
    {
      int col = k;
      int row = *(AtA.innerIndexPtr()+l);
      int value_index = l;
      assert(col < AtA.cols());
      assert(col >= 0);
      assert(row < AtA.rows());
      assert(row >= 0);
      assert(value_index >= 0);
      assert(value_index < AtA.nonZeros());

      data.I_outer.push_back(data.I_row.size());

      // Find correspondences
      unsigned i=0;
      unsigned j=0;
      while (i<Col_RowPtr[row].size() && j<Col_RowPtr[col].size())
      {
          if (Col_RowPtr[row][i] == Col_RowPtr[col][j])
          {
            data.I_row.push_back(Col_IndexPtr[row][i]);
            data.I_col.push_back(Col_IndexPtr[col][j]);
            data.I_w.push_back(Col_RowPtr[col][j]);
            ++i;
            ++j;
          } else 
          if (Col_RowPtr[row][i] > Col_RowPtr[col][j])
            ++j;
          else
            ++i;

      }
    }
  }
  data.I_outer.push_back(data.I_row.size()); // makes it more efficient to iterate later on

  igl::AtA_cached(A,data,AtA);
}

template <typename Scalar>
IGL_INLINE void igl::AtA_cached(
    const Eigen::SparseMatrix<Scalar>& A,
    const igl::AtA_cached_data& data,
    Eigen::SparseMatrix<Scalar>& AtA)
{
  for (unsigned i=0; i<data.I_outer.size()-1; ++i)
  {
    *(AtA.valuePtr() + i) = 0;
    for (unsigned j=data.I_outer[i]; j<data.I_outer[i+1]; ++j)
      *(AtA.valuePtr() + i) += *(A.valuePtr() + data.I_row[j]) * data.W[data.I_w[j]] * *(A.valuePtr() + data.I_col[j]);
  }
}


#ifdef IGL_STATIC_LIBRARY
template void igl::AtA_cached<double>(Eigen::SparseMatrix<double, 0, int> const&, igl::AtA_cached_data const&, Eigen::SparseMatrix<double, 0, int>&);
template void igl::AtA_cached_precompute<double>(Eigen::SparseMatrix<double, 0, int> const&, igl::AtA_cached_data&, Eigen::SparseMatrix<double, 0, int>&);
#endif
