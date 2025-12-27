// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BONE_PARENTS_H
#define IGL_BONE_PARENTS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // BONE_PARENTS Recover "parent" bones from directed graph representation.
  // 
  // Inputs:
  //   BE  #BE by 2 list of directed bone edges
  // Outputs:
  //   P  #BE by 1 list of parent indices into BE, -1 means root.
  //
  template <typename DerivedBE, typename DerivedP>
  IGL_INLINE void bone_parents(
    const Eigen::PlainObjectBase<DerivedBE>& BE,
    Eigen::PlainObjectBase<DerivedP>& P);
}

#ifndef IGL_STATIC_LIBRARY
#  include "bone_parents.cpp"
#endif

#endif

