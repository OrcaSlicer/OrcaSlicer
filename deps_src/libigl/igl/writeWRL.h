// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_WRITE_WRL_H
#define IGL_WRITE_WRL_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <string>
namespace igl
{
  // Write mesh to a .wrl file
  //
  // Inputs:
  //   str  path to .wrl file
  //   V  #V by 3 list of vertex positions
  //   F  #F by 3 list of triangle indices
  // Returns true iff succes
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE bool writeWRL(
    const std::string & str,
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::PlainObjectBase<DerivedF> & F);

  // Write mesh to a .wrl file
  //
  // Inputs:
  //   str  path to .wrl file
  //   V  #V by 3 list of vertex positions
  //   F  #F by 3 list of triangle indices
  //   C  double matrix of rgb values per vertex #V by 3
  // Returns true iff succes
  template <typename DerivedV, typename DerivedF, typename DerivedC>
  IGL_INLINE bool writeWRL(
    const std::string & str,
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::PlainObjectBase<DerivedF> & F,
    const Eigen::PlainObjectBase<DerivedC> & C);
}
#ifndef IGL_STATIC_LIBRARY
#include "writeWRL.cpp"
#endif
#endif
