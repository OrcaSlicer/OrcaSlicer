// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "../../slice_mask.h"
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#ifdef IGL_STATIC_LIBRARY
#undef IGL_STATIC_LIBRARY
#include "../../slice_mask.cpp"
template void igl::slice_mask<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3, 0,   -1, 3>, Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3, 0, -1, 3>   >(Eigen::DenseBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3, 0,   -1, 3> > const&, Eigen::Array<bool, -1, 1, 0, -1, 1> const&, int,   Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3,   0, -1, 3> >&);
#endif
