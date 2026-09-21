// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2019 Lawson Fulton lawsonfulton@gmail.com
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/
#ifndef IGL_PATH_TO_EDGES_H
#define IGL_PATH_TO_EDGES_H

#include "igl_inline.h"

#include <Eigen/Core>

#include <vector>

namespace igl
{
  /// Given a path as an ordered list of N>=2 vertex indices I[0], I[1], ..., I[N-1]
  /// construct a list of edges [[I[0],I[1]], [I[1],I[2]], ..., [I[N-2], I[N-1]]]
  /// connecting each sequential pair of vertices.
  ///
  /// @param[in] I  #I list of vertex indices
  /// @param[in] make_loop bool If true, include an edge connecting I[N-1] to I[0]
  /// @param[out] E  #I-1 by 2 list of edges
  /// 
  template <typename DerivedI, typename DerivedE>
  IGL_INLINE void path_to_edges(
    const Eigen::MatrixBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedE> & E,
    bool make_loop=false);
  /// \overload
  template <typename Index, typename DerivedE>
  IGL_INLINE void path_to_edges(
    const std::vector<Index> & I,
    Eigen::PlainObjectBase<DerivedE> & E,
    bool make_loop=false);
}
#ifndef IGL_STATIC_LIBRARY
#  include "path_to_edges.cpp"
#endif
#endif
