// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READ_TRIANGLE_MESH_H
#define IGL_READ_TRIANGLE_MESH_H
#include "igl_inline.h"

#include <Eigen/Core>
#include <string>
#include <cstdio>
#include <vector>
// History:
//  renamed read -> read_triangle_mesh     Daniele 24 June 2014
//  return type changed from void to bool  Alec 18 Sept 2011

namespace igl
{
  /// Read mesh from an ascii file with automatic detection of file format
  /// among: mesh, msh obj, off, ply, stl, wrl.
  /// 
  /// @tparam Scalar  type for positions and vectors (will be read as double and
  ///   cast to Scalar)
  /// @tparam Index  type for indices (will be read as int and cast to Index)
  /// @param[in] str  path to file
  /// @param[out] V  eigen double matrix #V by 3
  /// @param[out] F  eigen int matrix #F by 3
  /// @return true iff success
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE bool read_triangle_mesh(
    const std::string str,
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedF>& F);
  /// \overload
  /// \brief outputs to vectors, only .off and .obj supported.
  template <typename Scalar, typename Index>
  IGL_INLINE bool read_triangle_mesh(
    const std::string str,
    std::vector<std::vector<Scalar> > & V,
    std::vector<std::vector<Index> > & F);
  /// \overload
  /// @param[out] dir  directory path (see pathinfo.h)
  /// @param[out] base  base name (see pathinfo.h)
  /// @param[out] ext  extension (see pathinfo.h)
  /// @param[out] name  filename (see pathinfo.h)
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE bool read_triangle_mesh(
    const std::string str,
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedF>& F,
    std::string & dir,
    std::string & base,
    std::string & ext,
    std::string & name);
  /// \overload
  /// @param[in] ext  file extension
  /// @param[in,out] fp  pointer to already opened .ext file (will be closed)
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE bool read_triangle_mesh(
    const std::string & ext,
    FILE * fp,
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedF>& F);
}

#ifndef IGL_STATIC_LIBRARY
#  include "read_triangle_mesh.cpp"
#endif

#endif
