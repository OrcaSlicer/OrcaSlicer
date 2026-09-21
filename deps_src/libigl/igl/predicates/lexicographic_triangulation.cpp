// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2022 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "lexicographic_triangulation.h"
#include "../lexicographic_triangulation.h"
#include "predicates.h"

template<
  typename DerivedV,
  typename DerivedF
  >
IGL_INLINE void igl::predicates::lexicographic_triangulation(
    const Eigen::MatrixBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedF>& F)
{
  const auto orient2d = 
    [](const double *pa, const double *pb, const double *pc) 
  {
    Eigen::Vector2d pav; pav << pa[0], pa[1];
    Eigen::Vector2d pbv; pbv << pb[0], pb[1];
    Eigen::Vector2d pcv; pcv << pc[0], pc[1];
    return int(igl::predicates::orient2d(pav, pbv, pcv));
  };
  igl::lexicographic_triangulation(V, orient2d, F);
}

#ifdef STATIC_LIBRARY
template igl::predicates::lexicographic_triangulation<Eigen::MatrixXd,Eigen::MatrixXi>(const Eigen::MatrixXd &, Eigen::MatrixXi &);
#endif
