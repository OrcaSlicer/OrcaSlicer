// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_HARWELL_BOEING_H
#define IGL_HARWELL_BOEING_H
#include "igl_inline.h"

#include <Eigen/Sparse>
#include <vector>


namespace igl
{
  // Convert the matrix to Compressed sparse column (CSC or CCS) format,
  // also known as Harwell Boeing format. As described:
  // http://netlib.org/linalg/html_templates/node92.html
  // or
  // http://en.wikipedia.org/wiki/Sparse_matrix
  //   #Compressed_sparse_column_.28CSC_or_CCS.29
  // Templates:
  //   Scalar  type of sparse matrix like double
  // Inputs:
  //   A  sparse m by n matrix
  // Outputs:
  //   num_rows  number of rows
  //   V  non-zero values, row indices running fastest, size(V) = nnz 
  //   R  row indices corresponding to vals, size(R) = nnz
  //   C  index in vals of first entry in each column, size(C) = num_cols+1
  //
  // All indices and pointers are 0-based
  template <typename Scalar, typename Index>
  IGL_INLINE void harwell_boeing(
    const Eigen::SparseMatrix<Scalar> & A,
    int & num_rows,
    std::vector<Scalar> & V,
    std::vector<Index> & R,
    std::vector<Index> & C);
}

#ifndef IGL_STATIC_LIBRARY
#  include "harwell_boeing.cpp"
#endif

#endif
