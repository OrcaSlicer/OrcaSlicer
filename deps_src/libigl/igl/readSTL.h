// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// Copyright (C) 2020 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
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
#include <istream>
#include <vector>
#include <array>

namespace igl
{
  /// Read a mesh from an ascii/binary stl file.
  ///
  /// @tparam Scalar  type for positions and vectors (will be read as double and cast
  ///     to Scalar)
  /// @param[in] filename path to .stl file
  /// @param[out] V  double matrix of vertex positions  #V*3 by 3
  /// @param[out] F  index matrix of triangle indices #F by 3
  /// @param[out] N  double matrix of surface normals #F by 3
  /// @return true on success, false on errors
  ///
  /// #### Example
  ///
  ///     bool success = readSTL(filename,temp_V,F,N);
  ///     remove_duplicate_vertices(temp_V,0,V,SVI,SVJ);
  ///     for_each(F.data(),F.data()+F.size(),[&SVJ](int & f){f=SVJ(f);});
  ///     writeOBJ("Downloads/cat.obj",V,F);
  template <typename DerivedV, typename DerivedF, typename DerivedN>
  IGL_INLINE bool readSTL(
    std::istream &input,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedN> & N);
  /// \overload
  template <typename TypeV, typename TypeF, typename TypeN>
  IGL_INLINE bool readSTL(
    std::istream &input,
    std::vector<std::array<TypeV, 3> > & V,
    std::vector<std::array<TypeF, 3> > & F,
    std::vector<std::array<TypeN, 3> > & N);
  /// \overload
  /// @param[in,out] fp  pointer to ply file (will be closed)
  template <typename DerivedV, typename DerivedF, typename DerivedN>
  IGL_INLINE bool readSTL(
    FILE * fp,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedN> & N);
}

#ifndef IGL_STATIC_LIBRARY
#  include "readSTL.cpp"
#endif

#endif

