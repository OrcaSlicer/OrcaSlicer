// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READSTL_H
#define IGL_READSTL_H
#include "igl_inline.h"

#ifndef IGL_NO_EIGEN
#  include <Eigen/Core>
#endif
#include <string>
#include <cstdio>
#include <vector>

namespace igl 
{
  // Read a mesh from an ascii/binary stl file.
  //
  // Templates:
  //   Scalar  type for positions and vectors (will be read as double and cast
  //     to Scalar)
  // Inputs:
  //   filename path to .stl file
  // Outputs:
  //   V  double matrix of vertex positions  #V*3 by 3
  //   F  index matrix of triangle indices #F by 3
  //   N  double matrix of surface normals #F by 3
  // Returns true on success, false on errors
  //
  // Example:
  //   bool success = readSTL(filename,temp_V,F,N);
  //   remove_duplicate_vertices(temp_V,0,V,SVI,SVJ);
  //   for_each(F.data(),F.data()+F.size(),[&SVJ](int & f){f=SVJ(f);});
  //   writeOBJ("Downloads/cat.obj",V,F);
  template <typename DerivedV, typename DerivedF, typename DerivedN>
  IGL_INLINE bool readSTL(
    const std::string & filename,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedN> & N);
  // Inputs:
  //   stl_file  pointer to already opened .stl file 
  // Outputs:
  //   stl_file  closed file
  template <typename TypeV, typename TypeF, typename TypeN>
  IGL_INLINE bool readSTL(
    FILE * stl_file, 
    std::vector<std::vector<TypeV> > & V,
    std::vector<std::vector<TypeF> > & F,
    std::vector<std::vector<TypeN> > & N);
  template <typename TypeV, typename TypeF, typename TypeN>
  IGL_INLINE bool readSTL(
    const std::string & filename,
    std::vector<std::vector<TypeV> > & V,
    std::vector<std::vector<TypeF> > & F,
    std::vector<std::vector<TypeN> > & N);
}

#ifndef IGL_STATIC_LIBRARY
#  include "readSTL.cpp"
#endif

#endif

