// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "../../unique.h"
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#ifdef IGL_STATIC_LIBRARY
#undef IGL_STATIC_LIBRARY
#include "../../unique.cpp"
template void igl::unique<CGAL::Point_2<CGAL::Epeck> >(std::vector<CGAL::Point_2<CGAL::Epeck>, std::allocator<CGAL::Point_2<CGAL::Epeck> > > const&, std::vector<CGAL::Point_2<CGAL::Epeck>, std::allocator<CGAL::Point_2<CGAL::Epeck> > >&, std::vector<unsigned long, std::allocator<unsigned long> >&, std::vector<unsigned long, std::allocator<unsigned long> >&);
#endif
