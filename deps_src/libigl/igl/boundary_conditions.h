// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BOUNDARY_CONDITIONS_H
#define IGL_BOUNDARY_CONDITIONS_H
#include "igl_inline.h"
#include <Eigen/Dense>

namespace igl
{

  // Compute boundary conditions for automatic weights computation. This
  // function expects that the given mesh (V,Ele) has sufficient samples
  // (vertices) exactly at point handle locations and exactly along bone and
  // cage edges.
  //
  // Inputs:
  //   V  #V by dim list of domain vertices
  //   Ele  #Ele by simplex-size list of simplex indices
  //   C  #C by dim list of handle positions
  //   P  #P by 1 list of point handle indices into C
  //   BE  #BE by 2 list of bone edge indices into C
  //   CE  #CE by 2 list of cage edge indices into *P*
  // Outputs:
  //   b  #b list of boundary indices (indices into V of vertices which have
  //     known, fixed values)
  //   bc #b by #weights list of known/fixed values for boundary vertices
  //     (notice the #b != #weights in general because #b will include all the
  //     intermediary samples along each bone, etc.. The ordering of the
  //     weights corresponds to [P;BE]
  // Returns false if boundary conditions are suspicious:
  //   P and BE are empty
  //   bc is empty
  //   some column of bc doesn't have a 0 (assuming bc has >1 columns)
  //   some column of bc doesn't have a 1 (assuming bc has >1 columns)
  IGL_INLINE bool boundary_conditions(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & Ele,
    const Eigen::MatrixXd & C,
    const Eigen::VectorXi & P,
    const Eigen::MatrixXi & BE,
    const Eigen::MatrixXi & CE,
    Eigen::VectorXi & b,
    Eigen::MatrixXd & bc);
}

#ifndef IGL_STATIC_LIBRARY
#  include "boundary_conditions.cpp"
#endif

#endif
