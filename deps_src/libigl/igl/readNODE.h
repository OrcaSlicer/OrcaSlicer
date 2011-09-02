// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READNODE_H
#define IGL_READNODE_H
#include "igl_inline.h"

#include <string>
#include <vector>
#include <Eigen/Core>

namespace igl
{
  // load a list of points from a .node file
  //
  // Templates:
  //   Scalar  type for positions and vectors (will be read as double and cast
  //     to Scalar)
  //   Index  type for indices (will be read as int and cast to Index)
  // Input:
  //   node_file_name  path of .node file
  // Outputs:
  //   V  double matrix of vertex positions  #V by dim
  //   I  list of indices (first tells whether 0 or 1 indexed)
  template <typename Scalar, typename Index>
  IGL_INLINE bool readNODE(
    const std::string node_file_name,
    std::vector<std::vector<Scalar > > & V,
    std::vector<std::vector<Index > > & I);

  // Input:
  //   node_file_name  path of .node file
  // Outputs:
  //   V  eigen double matrix #V by dim
  template <typename DerivedV, typename DerivedI>
  IGL_INLINE bool readNODE(
    const std::string node_file_name,
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedI>& I);
}

#ifndef IGL_STATIC_LIBRARY
#  include "readNODE.cpp"
#endif

#endif
