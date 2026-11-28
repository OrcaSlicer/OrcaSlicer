// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ROWS_TO_MATRIX_H
#define IGL_ROWS_TO_MATRIX_H
#include "igl_inline.h"
#include <vector>
namespace igl
{
  /// Convert a list (std::vector) of row vectors of the same length to a matrix
  /// @tparam Row  row vector type, must implement:
  ///     .size()
  /// @tparam Mat  Matrix type, must implement:
  ///     .resize(m,n)
  ///     .row(i) = Row
  /// @param[in] V  a m-long list of vectors of size n
  /// @param[out] M  an m by n matrix
  /// @return true on success, false on errors
  template <class Row, class Mat>
  IGL_INLINE bool rows_to_matrix(const std::vector<Row> & V,Mat & M);
}

#ifndef IGL_STATIC_LIBRARY
#  include "rows_to_matrix.cpp"
#endif

#endif
