// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "barycentric_to_global.h"

// For error printing
#include <cstdio>
#include <vector>

namespace igl
{
  template <typename Scalar, typename Index>
  IGL_INLINE Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> barycentric_to_global(
    const Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> & V,
    const Eigen::Matrix<Index,Eigen::Dynamic,Eigen::Dynamic>  & F,
    const Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> & bc)
  {
    Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> R;
    R.resize(bc.rows(),3);

    for (unsigned i=0; i<R.rows(); ++i)
    {
      unsigned id = round(bc(i,0));
      double u   = bc(i,1);
      double v   = bc(i,2);

      if (id != -1)
        R.row(i) = V.row(F(id,0)) +
                  ((V.row(F(id,1)) - V.row(F(id,0))) * u +
                   (V.row(F(id,2)) - V.row(F(id,0))) * v  );
      else
        R.row(i) << 0,0,0;
    }
    return R;
  }
}

#ifdef IGL_STATIC_LIBRARY
template Eigen::Matrix<double, -1, -1, 0, -1, -1> igl::barycentric_to_global<double, int>(Eigen::Matrix<double, -1, -1, 0, -1, -1> const&, Eigen::Matrix<int, -1, -1, 0, -1, -1> const&, Eigen::Matrix<double, -1, -1, 0, -1, -1> const&);
#endif
