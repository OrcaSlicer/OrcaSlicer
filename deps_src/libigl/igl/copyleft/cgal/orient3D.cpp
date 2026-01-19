// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Qingan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#include "orient3D.h"
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>

template<typename Scalar>
IGL_INLINE short igl::copyleft::cgal::orient3D(
    const Scalar pa[3],
    const Scalar pb[3],
    const Scalar pc[3],
    const Scalar pd[3])
{
  typedef CGAL::Exact_predicates_exact_constructions_kernel Epeck;
  typedef CGAL::Exact_predicates_inexact_constructions_kernel Epick;
  typedef typename std::conditional<std::is_same<Scalar, Epeck::FT>::value,
          Epeck, Epick>::type Kernel;

  switch(CGAL::orientation(
        typename Kernel::Point_3(pa[0], pa[1], pa[2]),
        typename Kernel::Point_3(pb[0], pb[1], pb[2]),
        typename Kernel::Point_3(pc[0], pc[1], pc[2]),
        typename Kernel::Point_3(pd[0], pd[1], pd[2]))) {
    case CGAL::POSITIVE:
      return 1;
    case CGAL::NEGATIVE:
      return -1;
    case CGAL::COPLANAR:
      return 0;
    default:
      throw "Invalid orientation";
  }
}
