// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BLKDIAG_H
#define IGL_BLKDIAG_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
#include <vector>

namespace igl
{
  /// Given a list of matrices place them along the diagonal as blocks of the
  /// output matrix. Like matlab's blkdiag.
  ///
  /// @param[in] L  list of matrices {A,B, ...}
  /// @param[out] Y  A.rows()+B.rows()+... by A.cols()+B.cols()+... block diagonal
  ///
  /// \see 
  ///   cat, 
  ///   repdiag
  template <typename Scalar>
  IGL_INLINE void blkdiag(
    const std::vector<Eigen::SparseMatrix<Scalar>> & L, 
    Eigen::SparseMatrix<Scalar> & Y);
  /// \overload
  template <typename DerivedY>
  IGL_INLINE void blkdiag(
    const std::vector<DerivedY> & L, 
    Eigen::PlainObjectBase<DerivedY> & Y);
}

#ifndef IGL_STATIC_LIBRARY
#  include "blkdiag.cpp"
#endif

#endif
