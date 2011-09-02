// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_AVERAGEEDGELENGTH_H
#define IGL_AVERAGEEDGELENGTH_H

#include "igl_inline.h"
#include <Eigen/Core>
#include <string>
#include <vector>

namespace igl 
{
  // Compute the average edge length for the given triangle mesh
  // Templates:
  //   DerivedV derived from vertex positions matrix type: i.e. MatrixXd
  //   DerivedF derived from face indices matrix type: i.e. MatrixXi
  //   DerivedL derived from edge lengths matrix type: i.e. MatrixXd
  // Inputs:
  //   V  eigen matrix #V by 3
  //   F  #F by simplex-size list of mesh faces (must be simplex)
  // Outputs:
  //   l  average edge length
  //
  // See also: adjacency_matrix
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE double avg_edge_length(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F);

}

#ifndef IGL_STATIC_LIBRARY
#  include "avg_edge_length.cpp"
#endif

#endif
