// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READOFF_H
#define IGL_READOFF_H
#include "igl_inline.h"
// History:
//  return type changed from void to bool  Alec 18 Sept 2011

#ifndef IGL_NO_EIGEN
#  include <Eigen/Core>
#endif
#include <string>
#include <vector>
#include <cstdio>

namespace igl 
{
  /// Read a mesh from an ascii OFF file, filling in vertex positions, normals
  /// and texture coordinates. Mesh may have faces of any number of degree
  ///
  /// @tparam Scalar  type for positions and vectors (will be read as double and cast
  ///     to Scalar)
  /// @tparam Index  type for indices (will be read as int and cast to Index)
  /// @param[in] str  path to .obj file
  /// @param[out] V  double matrix of vertex positions  #V by 3
  /// @param[out] F  #F list of face indices into vertex positions
  /// @param[out] N  list of vertex normals #V by 3
  /// @param[out] C  list of rgb color values per vertex #V by 3
  /// @return true on success, false on errors
  template <typename Scalar, typename Index>
  IGL_INLINE bool readOFF(
    const std::string off_file_name, 
    std::vector<std::vector<Scalar > > & V,
    std::vector<std::vector<Index > > & F,
    std::vector<std::vector<Scalar > > & N,
    std::vector<std::vector<Scalar > > & C);
  /// \overload
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE bool readOFF(
    const std::string str,
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedF>& F);
  /// \overload
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE bool readOFF(
    const std::string str,
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedV>& N);
  /// \overload
  /// @param[in,out] off_file  pointer to already open .off file (will be closed)
  template <typename Scalar, typename Index>
  IGL_INLINE bool readOFF(
    FILE * off_file,
    std::vector<std::vector<Scalar > > & V,
    std::vector<std::vector<Index > > & F,
    std::vector<std::vector<Scalar > > & N,
    std::vector<std::vector<Scalar > > & C);

}

#ifndef IGL_STATIC_LIBRARY
#  include "readOFF.cpp"
#endif

#endif
