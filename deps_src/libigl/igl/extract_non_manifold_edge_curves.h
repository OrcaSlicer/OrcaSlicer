// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_NON_MANIFOLD_EDGE_CURVES
#define IGL_NON_MANIFOLD_EDGE_CURVES

#include "igl_inline.h"
#include <Eigen/Dense>
#include <vector>

namespace igl {
    // Extract non-manifold curves from a given mesh.
    // A non-manifold curves are a set of connected non-manifold edges that
    // does not touch other non-manifold edges except at the end points.
    // They are also maximal in the sense that they cannot be expanded by
    // including more edges.
    //
    // Assumes the input mesh have all self-intersection resolved.  See
    // ``igl::cgal::remesh_self_intersection`` for more details.
    //
    // Inputs:
    //   F  #F by 3 list representing triangles.
    //   EMAP  #F*3 list of indices of unique undirected edges.
    //   uE2E  #uE list of lists of indices into E of coexisting edges.
    //
    // Output:
    //   curves  An array of arries of unique edge indices.
    template<
        typename DerivedF,
        typename DerivedEMAP,
        typename uE2EType>
    IGL_INLINE void extract_non_manifold_edge_curves(
            const Eigen::PlainObjectBase<DerivedF>& F,
            const Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
            const std::vector<std::vector<uE2EType> >& uE2E,
            std::vector<std::vector<size_t> >& curves);
}

#ifndef IGL_STATIC_LIBRARY
#  include "extract_non_manifold_edge_curves.cpp"
#endif

#endif
