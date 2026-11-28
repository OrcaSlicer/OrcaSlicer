// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PRINT_IJV_H
#define IGL_PRINT_IJV_H
#include "igl_inline.h"
#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>
namespace igl
{
  /// Prints a 3 column matrix representing [I,J,V] = find(X). That is, each
  /// row is the row index, column index and value for each non zero entry. Each
  /// row is printed on a new line
  ///
  /// @tparam T  should be a eigen sparse matrix primitive type like int or double
  /// @param[in] X  m by n matrix whose entries are to be sorted
  /// @param[in] offset  optional offset for I and J indices {0}
  ///
  /// \see matlab_format
  template <typename T>
  IGL_INLINE void print_ijv(
    const Eigen::SparseMatrix<T>& X, 
    const int offset=0);
}

#ifndef IGL_STATIC_LIBRARY
#  include "print_ijv.cpp"
#endif

#endif
