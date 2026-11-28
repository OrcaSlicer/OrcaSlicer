// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//                    Qingan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#include "lexicographic_triangulation.h"
#include "../../lexicographic_triangulation.h"
#include "orient2D.h"

template<
  typename DerivedP,
  typename DerivedF
  >
IGL_INLINE void igl::copyleft::cgal::lexicographic_triangulation(
    const Eigen::MatrixBase<DerivedP>& P,
    Eigen::PlainObjectBase<DerivedF>& F)
{
  typedef typename DerivedP::Scalar Scalar;
  igl::lexicographic_triangulation(P, orient2D<Scalar>, F);
}
