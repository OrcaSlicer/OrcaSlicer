// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READPLY_H
#define IGL_READPLY_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <string>
#include <vector>
#include <cstdio>

namespace igl
{
  // Read a mesh from a .ply file. 
  //
  // Inputs:
  //   filename  path to .ply file
  // Outputs:
  //   V  #V by 3 list of vertex positions
  //   F  #F list of lists of triangle indices
  //   N  #V by 3 list of vertex normals
  //   UV  #V by 2 list of vertex texture coordinates
  // Returns true iff success
  template <
    typename Vtype,
    typename Ftype,
    typename Ntype,
    typename UVtype>
  IGL_INLINE bool readPLY(
    const std::string filename,
    std::vector<std::vector<Vtype> > & V,
    std::vector<std::vector<Ftype> > & F,
    std::vector<std::vector<Ntype> > & N,
    std::vector<std::vector<UVtype> >  & UV);
  template <
    typename Vtype,
    typename Ftype,
    typename Ntype,
    typename UVtype>
  // Inputs:
  //   ply_file  pointer to already opened .ply file 
  // Outputs:
  //   ply_file  closed file
  IGL_INLINE bool readPLY(
    FILE * ply_file,
    std::vector<std::vector<Vtype> > & V,
    std::vector<std::vector<Ftype> > & F,
    std::vector<std::vector<Ntype> > & N,
    std::vector<std::vector<UVtype> >  & UV);
    template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedN,
    typename DerivedUV>
  IGL_INLINE bool readPLY(
    const std::string filename,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedN> & N,
    Eigen::PlainObjectBase<DerivedUV> & UV);
  template <
    typename DerivedV,
    typename DerivedF>
  IGL_INLINE bool readPLY(
    const std::string filename,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F);
}
#ifndef IGL_STATIC_LIBRARY
#  include "readPLY.cpp"
#endif
#endif

