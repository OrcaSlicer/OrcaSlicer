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
  const Eigen::MatrixBase<DerivedV> & V,
  const typename DerivedV::Index & i)
{
  return CGAL::Point_2<Kernel>(V(i,0),V(i,1));
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
template CGAL::Point_2<CGAL::Epeck> igl::copyleft::cgal::row_to_point<CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>::Index const&);
#endif
