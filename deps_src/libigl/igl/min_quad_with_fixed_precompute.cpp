// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "min_quad_with_fixed.impl.h"

#ifdef IGL_STATIC_LIBRARY
template bool igl::min_quad_with_fixed_precompute<double, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::SparseMatrix<double, 0, int> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::SparseMatrix<double, 0, int> const&, bool, igl::min_quad_with_fixed_data<double>&);
template bool igl::min_quad_with_fixed_precompute<double, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::SparseMatrix<double, 0, int> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int> const&, bool, igl::min_quad_with_fixed_data<double>&);
#endif
