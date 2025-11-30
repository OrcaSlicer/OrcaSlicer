// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READWRL_H
#define IGL_READWRL_H
#include "igl_inline.h"

#include <string>
#include <vector>
#include <cstdio>

namespace igl 
{
  /// Read a mesh from an ascii wrl file, filling in vertex positions and face
  /// indices of the first model. Mesh may have faces of any number of degree
  ///
  /// @tparam Scalar  type for positions and vectors (will be read as double and cast
  ///     to Scalar)
  /// @tparam Index  type for indices (will be read as int and cast to Index)
  /// @param[in] str  path to .wrl file
  /// @param[out] V  double matrix of vertex positions  #V by 3
  /// @param[out] F  #F list of face indices into vertex positions
  /// @return true on success, false on errors
  template <typename Scalar, typename Index>
  IGL_INLINE bool readWRL(
    const std::string wrl_file_name, 
    std::vector<std::vector<Scalar > > & V,
    std::vector<std::vector<Index > > & F);
  /// \overload
  /// @param[in,out] wrl_file  pointer to already opened .wrl file  (will be closed)
  template <typename Scalar, typename Index>
  IGL_INLINE bool readWRL(
    FILE * wrl_file,
    std::vector<std::vector<Scalar > > & V,
    std::vector<std::vector<Index > > & F);

}

#ifndef IGL_STATIC_LIBRARY
#  include "readWRL.cpp"
#endif

#endif

