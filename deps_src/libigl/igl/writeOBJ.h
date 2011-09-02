// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_WRITEOBJ_H
#define IGL_WRITEOBJ_H
#include "igl_inline.h"
// History:
//  return type changed from void to bool  Alec 20 Sept 2011

#include <Eigen/Core>
#include <string>

namespace igl 
{
  // Write a mesh in an ascii obj file
  // Inputs:
  //   str  path to outputfile
  //   V  #V by 3 mesh vertex positions
  //   F  #F by 3|4 mesh indices into V
  //   CN #CN by 3 normal vectors
  //   FN  #F by 3|4 corner normal indices into CN
  //   TC  #TC by 2|3 texture coordinates
  //   FTC #F by 3|4 corner texture coord indices into TC
  // Returns true on success, false on error
  //
  // Known issues: Horrifyingly, this does not have the same order of
  // parameters as readOBJ.
  template <
    typename DerivedV, 
    typename DerivedF,
    typename DerivedCN, 
    typename DerivedFN,
    typename DerivedTC, 
    typename DerivedFTC>
  IGL_INLINE bool writeOBJ(
    const std::string str,
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedCN>& CN,
    const Eigen::MatrixBase<DerivedFN>& FN,
    const Eigen::MatrixBase<DerivedTC>& TC,
    const Eigen::MatrixBase<DerivedFTC>& FTC);
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE bool writeOBJ(
    const std::string str,
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F);

}

#ifndef IGL_STATIC_LIBRARY
#  include "writeOBJ.cpp"
#endif

#endif
