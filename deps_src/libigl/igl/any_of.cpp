// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "any_of.h"
#include <Eigen/Core>
template <typename Mat>
IGL_INLINE bool igl::any_of(const Mat & S)
{
  return std::any_of(S.data(),S.data()+S.size(),[](bool s){return s;});
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::any_of<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::Matrix<int, -1, 1, 0, -1, 1> const&);
#endif

