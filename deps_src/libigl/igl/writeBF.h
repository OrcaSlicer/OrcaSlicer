// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_WRITEBF_H
#define IGL_WRITEBF_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <string>
namespace igl
{
  /// Write a bones forest to a file
  ///
  /// @param[in] file_name  path to .bf bones tree file
  /// @param[in] WI  #B list of unique weight indices
  /// @param[in] P  #B list of parent indices into B, -1 for roots
  /// @param[in] O  #B list of tip offsets
  /// @return true on success, false on errors
  ///
  /// \see readBF
  template < 
    typename DerivedWI,
    typename DerivedP,
    typename DerivedO>
  IGL_INLINE bool writeBF(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedWI> & WI,
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedO> & O);
}

#ifndef IGL_STATIC_LIBRARY
#  include "writeBF.cpp"
#endif
#endif
