// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "row_to_point.h"

template <
  typename Kernel,
  typename DerivedV>
IGL_INLINE CGAL::Point_2<Kernel> igl::copyleft::cgal::row_to_point(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const typename DerivedV::Index & i)
{
  return CGAL::Point_2<Kernel>(V(i,0),V(i,1));
}
