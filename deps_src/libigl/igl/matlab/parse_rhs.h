// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MATLAB_PARSE_RHS_H
#define IGL_MATLAB_PARSE_RHS_H
#include <igl/igl_inline.h>
#include <mex.h>
#include <Eigen/Dense>
#include <Eigen/Sparse>
namespace igl
{
  namespace matlab
  {
    // Reads in a matrix as a double
    //
    // Inputs:
    //   prhs  points to rhs argument
    // Outputs:
    //   V  M by N matrix 
    template <typename DerivedV>
    IGL_INLINE void parse_rhs_double(
      const mxArray *prhs[], 
      Eigen::PlainObjectBase<DerivedV> & V);
    // Reads in a matrix and subtracts 1
    template <typename DerivedV>
    IGL_INLINE void parse_rhs_index(
      const mxArray *prhs[], 
      Eigen::PlainObjectBase<DerivedV> & V);
    template <typename VType>
    IGL_INLINE void parse_rhs(
      const mxArray *prhs[], 
      Eigen::SparseMatrix<VType> & M);
  }
};
#ifndef IGL_STATIC_LIBRARY
#  include "parse_rhs.cpp"
#endif
#endif
