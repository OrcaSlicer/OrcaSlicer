// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "bounding_box_diagonal.h"
#include "max.h"
#include "min.h"
#include <cmath>

IGL_INLINE double igl::bounding_box_diagonal(
  const Eigen::MatrixXd & V)
{
  using namespace Eigen;
  VectorXd maxV,minV;
  VectorXi maxVI,minVI;
  igl::max(V,1,maxV,maxVI);
  igl::min(V,1,minV,minVI);
  return sqrt((maxV-minV).array().square().sum());
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
