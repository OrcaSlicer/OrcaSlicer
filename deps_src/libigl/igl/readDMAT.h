// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READDMAT_H
#define IGL_READDMAT_H
#include "igl_inline.h"
// .dmat is a simple ascii matrix file type, defined as follows. The first line
// is always:
// <#columns> <#rows>
// Then the coefficients of the matrix are given separated by whitespace with
// columns running fastest.
//
// Example:
//   The matrix m = [1 2 3; 4 5 6];
//   corresponds to a .dmat file containing:
//   3 2
//   1 4 2 5 3 6
#include <string>
#include <vector>
#ifndef IGL_NO_EIGEN
#  include <Eigen/Core>
#endif
namespace igl
{
  // Read a matrix from an ascii dmat file
  //
  // Inputs:
  //   file_name  path to .dmat file
  // Outputs:
  //   W  eigen matrix containing read-in coefficients
  // Returns true on success, false on error
  //
#ifndef IGL_NO_EIGEN
  template <typename DerivedW>
  IGL_INLINE bool readDMAT(const std::string file_name, 
    Eigen::PlainObjectBase<DerivedW> & W);
#endif
  // Wrapper for vector of vectors
  template <typename Scalar>
  IGL_INLINE bool readDMAT(
    const std::string file_name, 
    std::vector<std::vector<Scalar> > & W);
}

#ifndef IGL_STATIC_LIBRARY
#  include "readDMAT.cpp"
#endif

#endif
