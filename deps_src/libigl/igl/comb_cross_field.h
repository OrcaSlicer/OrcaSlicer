// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_COMB_CROSS_FIELD_H
#define IGL_COMB_CROSS_FIELD_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Computes principal matchings of the vectors of a cross field across face edges,
  // and generates a combed cross field defined on the mesh faces
  
  // Inputs:
  //   V          #V by 3 eigen Matrix of mesh vertex 3D positions
  //   F          #F by 4 eigen Matrix of face (quad) indices
  //   PD1in      #F by 3 eigen Matrix of the first per face cross field vector
  //   PD2in      #F by 3 eigen Matrix of the second per face cross field vector
  // Output:
  //   PD1out      #F by 3 eigen Matrix of the first combed cross field vector
  //   PD2out      #F by 3 eigen Matrix of the second combed cross field vector
  //
  
  
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE void comb_cross_field(const Eigen::PlainObjectBase<DerivedV> &V,
                                   const Eigen::PlainObjectBase<DerivedF> &F,
                                   const Eigen::PlainObjectBase<DerivedV> &PD1in,
                                   const Eigen::PlainObjectBase<DerivedV> &PD2in,
                                   Eigen::PlainObjectBase<DerivedV> &PD1out,
                                   Eigen::PlainObjectBase<DerivedV> &PD2out);
}
#ifndef IGL_STATIC_LIBRARY
#include "comb_cross_field.cpp"
#endif

#endif
