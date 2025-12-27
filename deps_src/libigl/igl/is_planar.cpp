// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "is_planar.h"
IGL_INLINE bool igl::is_planar(const Eigen::MatrixXd & V)
{
  if(V.size() == 0) return false;
  if(V.cols() == 2) return true;
  for(int i = 0;i<V.rows();i++)
  {
    if(V(i,2) != 0) return false;
  }
  return true;
}
