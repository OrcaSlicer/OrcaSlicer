// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "../../list_to_matrix.h"
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#ifdef IGL_STATIC_LIBRARY
#undef IGL_STATIC_LIBRARY
#include "../../list_to_matrix.cpp"
template bool igl::list_to_matrix<CGAL::Epeck::FT, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >(std::vector<std::vector<CGAL::Epeck::FT, std::allocator<CGAL::Epeck::FT > >, std::allocator<std::vector<CGAL::Epeck::FT, std::allocator<CGAL::Epeck::FT > > > > const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&);
#endif
