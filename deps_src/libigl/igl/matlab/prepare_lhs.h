// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MATLAB_PREPARE_LHS_H
#define IGL_MATLAB_PREPARE_LHS_H
#include <igl/igl_inline.h>
#include <mex.h>
#include <Eigen/Dense>
#include <Eigen/Sparse>
namespace igl
{
  namespace matlab
  {
    // Writes out a matrix as a double
    //
    // Inputs:
    //   prhs  points to rhs argument
    // Outputs:
    //   V  M by N matrix 
    template <typename DerivedV>
    IGL_INLINE void prepare_lhs_double(
      const Eigen::PlainObjectBase<DerivedV> & V,
      mxArray *plhs[]);
    // Casts to logical
    template <typename DerivedV>
    IGL_INLINE void prepare_lhs_logical(
      const Eigen::PlainObjectBase<DerivedV> & V,
      mxArray *plhs[]);
    // Writes out a matrix and adds 1
    template <typename DerivedV>
    IGL_INLINE void prepare_lhs_index(
      const Eigen::PlainObjectBase<DerivedV> & V,
      mxArray *plhs[]);
    // SparseMatrix
    template <typename Vtype>
    IGL_INLINE void prepare_lhs_double(
      const Eigen::SparseMatrix<Vtype> & V,
      mxArray *plhs[]);
  };
}
#ifndef IGL_STATIC_LIBRARY
#  include "prepare_lhs.cpp"
#endif
#endif

