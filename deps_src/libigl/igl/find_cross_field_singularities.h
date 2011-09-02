// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_FIND_CROSS_FIELD_SINGULARITIES_H
#define IGL_FIND_CROSS_FIELD_SINGULARITIES_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Computes singularities of a cross field, assumed combed


  // Inputs:
  //   V                #V by 3 eigen Matrix of mesh vertex 3D positions
  //   F                #F by 3 eigen Matrix of face (quad) indices
  //   Handle_MMatch    #F by 3 eigen Matrix containing the integer missmatch of the cross field
  //                    across all face edges
  // Output:
  //   isSingularity    #V by 1 boolean eigen Vector indicating the presence of a singularity on a vertex
  //   singularityIndex #V by 1 integer eigen Vector containing the singularity indices
  //
  template <typename DerivedV, typename DerivedF, typename DerivedM, typename DerivedO>
  IGL_INLINE void find_cross_field_singularities(const Eigen::PlainObjectBase<DerivedV> &V,
                                                 const Eigen::PlainObjectBase<DerivedF> &F,
                                                 const Eigen::PlainObjectBase<DerivedM> &Handle_MMatch,
                                                 Eigen::PlainObjectBase<DerivedO> &isSingularity,
                                                 Eigen::PlainObjectBase<DerivedO> &singularityIndex);

  // Wrapper that calculates the missmatch if it is not provided.
  // Note that the field in PD1 and PD2 MUST BE combed (see igl::comb_cross_field).
  // Inputs:
  //   V                #V by 3 eigen Matrix of mesh vertex 3D positions
  //   F                #F by 3 eigen Matrix of face (quad) indices
  //   PD1              #F by 3 eigen Matrix of the first per face cross field vector
  //   PD2              #F by 3 eigen Matrix of the second per face  cross field vector
  // Output:
  //   isSingularity    #V by 1 boolean eigen Vector indicating the presence of a singularity on a vertex
  //   singularityIndex #V by 1 integer eigen Vector containing the singularity indices
  //
  template <typename DerivedV, typename DerivedF, typename DerivedO>
  IGL_INLINE void find_cross_field_singularities(const Eigen::PlainObjectBase<DerivedV> &V,
                                                 const Eigen::PlainObjectBase<DerivedF> &F,
                                                 const Eigen::PlainObjectBase<DerivedV> &PD1,
                                                 const Eigen::PlainObjectBase<DerivedV> &PD2,
                                                 Eigen::PlainObjectBase<DerivedO> &isSingularity,
                                                 Eigen::PlainObjectBase<DerivedO> &singularityIndex,
                                                 bool isCombed = false);
}
#ifndef IGL_STATIC_LIBRARY
#include "find_cross_field_singularities.cpp"
#endif

#endif
