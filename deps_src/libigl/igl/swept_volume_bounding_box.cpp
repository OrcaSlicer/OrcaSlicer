// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "swept_volume_bounding_box.h"
#include "LinSpaced.h"

IGL_INLINE void igl::swept_volume_bounding_box(
  const size_t & n,
  const std::function<Eigen::RowVector3d(const size_t vi, const double t)> & V,
  const size_t & steps,
  Eigen::AlignedBox3d & box)
{
  using namespace Eigen;
  box.setEmpty();
  const VectorXd t = igl::LinSpaced<VectorXd >(steps,0,1);
  // Find extent over all time steps
  for(int ti = 0;ti<t.size();ti++)
  {
    for(size_t vi = 0;vi<n;vi++)
    {
      box.extend(V(vi,t(ti)).transpose());
    }
  }
}
