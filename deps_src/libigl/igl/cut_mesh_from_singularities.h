// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_CUT_MESH_FROM_SINGULARITIES_H
#define IGL_CUT_MESH_FROM_SINGULARITIES_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Given a mesh (V,F) and the integer mismatch of a cross field per edge
  // (MMatch), finds the cut_graph connecting the singularities (seams) and the
  // degree of the singularities singularity_index
  //
  // Input:
  //   V  #V by 3 list of mesh vertex positions
  //   F  #F by 3 list of faces
  //   MMatch  #F by 3 list of per corner integer mismatch
  // Outputs:
  //   seams  #F by 3 list of per corner booleans that denotes if an edge is a
  //     seam or not
  //
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedM, 
    typename DerivedO> 
  IGL_INLINE void cut_mesh_from_singularities(
    const Eigen::PlainObjectBase<DerivedV> &V, 
    const Eigen::PlainObjectBase<DerivedF> &F, 
    const Eigen::PlainObjectBase<DerivedM> &MMatch,
    Eigen::PlainObjectBase<DerivedO> &seams);
}
#ifndef IGL_STATIC_LIBRARY
#include "cut_mesh_from_singularities.cpp"
#endif

#endif
