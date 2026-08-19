// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "min_quad_with_fixed.impl.h"

#ifdef IGL_STATIC_LIBRARY
template Eigen::Matrix<float, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> igl::min_quad_with_fixed<float, 3, 0, true>(Eigen::Matrix<float, 3, 3, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)1) : ((((3) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 3> const&, Eigen::Matrix<float, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> const&, Eigen::Array<bool, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> const&, Eigen::Matrix<float, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> const&, Eigen::Matrix<float, 0, 3, ((Eigen::StorageOptions)0) | ((((0) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)1) : ((((3) == (1)) && ((0) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 0, 3> const&, Eigen::Matrix<float, 0, 1, ((Eigen::StorageOptions)0) | ((((0) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((0) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 0, 1> const&);
template bool igl::min_quad_with_fixed_solve<double, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>>(igl::min_quad_with_fixed_data<double> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>>&);
#endif


