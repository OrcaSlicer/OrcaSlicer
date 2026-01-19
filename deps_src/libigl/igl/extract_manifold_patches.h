// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EXTRACT_MANIFOLD_PATCHES
#define IGL_EXTRACT_MANIFOLD_PATCHES

#include "igl_inline.h"
#include <Eigen/Dense>
#include <vector>

namespace igl {
    // Extract a set of maximal patches from a given mesh.
    // A maximal patch is a subset of the input faces that are connected via
    // manifold edges; a patch is as large as possible.
    //
    // Inputs:
    //   F  #F by 3 list representing triangles.
    //   EMAP  #F*3 list of indices of unique undirected edges.
    //   uE2E  #uE list of lists of indices into E of coexisting edges.
    //
    // Output:
    //   P  #F list of patch incides.
    //
    // Returns:
    //   number of manifold patches.
    template <
      typename DerivedF,
      typename DerivedEMAP,
      typename uE2EType,
      typename DerivedP>
    IGL_INLINE size_t extract_manifold_patches(
      const Eigen::PlainObjectBase<DerivedF>& F,
      const Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
      const std::vector<std::vector<uE2EType> >& uE2E,
      Eigen::PlainObjectBase<DerivedP>& P);
    template <
      typename DerivedF,
      typename DerivedP>
    IGL_INLINE size_t extract_manifold_patches(
      const Eigen::PlainObjectBase<DerivedF>& F,
      Eigen::PlainObjectBase<DerivedP>& P);
}
#ifndef IGL_STATIC_LIBRARY
#  include "extract_manifold_patches.cpp"
#endif

#endif
