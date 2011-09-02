// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IS_BOUNDARY_EDGE_H
#define IS_BOUNDARY_EDGE_H
#include <Eigen/Dense>

namespace igl
{
  //  IS_BOUNDARY_EDGE Determine for each edge E if it is a "boundary edge" in F.
  //  Boundary edges are undirected edges which occur only once.
  // 
  //  Inputs:
  //    E  #E by 2 list of edges
  //    F  #F by 3 list of triangles
  //  Outputs:
  //    B  #E list bools. true iff unoriented edge occurs exactly once in F
  //      (non-manifold and non-existant edges will be false)
  // 
  template <
    typename DerivedF,
    typename DerivedE,
    typename DerivedB>
  void is_boundary_edge(
    const Eigen::PlainObjectBase<DerivedE> & E,
    const Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedB> & B);
  // Wrapper where Edges should also be computed from F
  //   E  #E by 2 list of edges
  //   EMAP  #F*3 list of indices mapping allE to E
  template <
    typename DerivedF,
    typename DerivedE,
    typename DerivedB,
    typename DerivedEMAP>
  void is_boundary_edge(
    const Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedB> & B,
    Eigen::PlainObjectBase<DerivedE> & E,
    Eigen::PlainObjectBase<DerivedEMAP> & EMAP);
}

#ifndef IGL_STATIC_LIBRARY
#  include "is_boundary_edge.cpp"
#endif

#endif
