// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_WRITEPLY_H
#define IGL_WRITEPLY_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <string>

namespace igl
{
  // Write a mesh to a .ply file. 
  //
  // Inputs:
  //   filename  path to .ply file
  //   V  #V by 3 list of vertex positions
  //   F  #F by 3 list of triangle indices
  //   N  #V by 3 list of vertex normals
  //   UV  #V by 2 list of vertex texture coordinates
  // Returns true iff success
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedN,
    typename DerivedUV>
  IGL_INLINE bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedN> & N,
    const Eigen::MatrixBase<DerivedUV> & UV,
    const bool ascii = true);
  template <
    typename DerivedV,
    typename DerivedF>
  IGL_INLINE bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const bool ascii = true);
}
#ifndef IGL_STATIC_LIBRARY
#  include "writePLY.cpp"
#endif
#endif
