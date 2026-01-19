// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DIRECTED_EDGE_PARENTS_H
#define IGL_DIRECTED_EDGE_PARENTS_H
#include "igl_inline.h"

#include <Eigen/Dense>

namespace igl
{
  // Recover "parents" (preceding edges) in a tree given just directed edges.
  //
  // Inputs:
  //   E  #E by 2 list of directed edges
  // Outputs:
  //   P  #E list of parent indices into E (-1) means root
  //
  template <typename DerivedE, typename DerivedP>
  IGL_INLINE void directed_edge_parents(
    const Eigen::PlainObjectBase<DerivedE> & E,
    Eigen::PlainObjectBase<DerivedP> & P);
}

#ifndef IGL_STATIC_LIBRARY
#  include "directed_edge_parents.cpp"
#endif
#endif
