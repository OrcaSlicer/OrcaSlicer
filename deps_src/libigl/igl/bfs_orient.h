// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BFS_ORIENT_H
#define IGL_BFS_ORIENT_H
#include <Eigen/Core>
#include "igl_inline.h"

namespace igl
{
  /// Consistently orient faces in orientable patches using BFS.
  ///
  /// @param[in] F  #F by 3 list of faces
  /// @param[out] FF  #F by 3 list of faces (OK if same as F)
  /// @param[out] C  #F list of component ids
  ///
  template <typename DerivedF, typename DerivedFF, typename DerivedC>
  IGL_INLINE void bfs_orient(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedFF> & FF,
    Eigen::PlainObjectBase<DerivedC> & C);
}
#ifndef IGL_STATIC_LIBRARY
#  include "bfs_orient.cpp"
#endif

#endif
