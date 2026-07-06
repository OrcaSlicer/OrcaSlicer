// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DIRECTED_EDGE_ORIENTATIONS_H
#define IGL_DIRECTED_EDGE_ORIENTATIONS_H
#include "igl_inline.h"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/StdVector>
#include <vector>

namespace igl
{
  /// Determine rotations that take each edge from the x-axis to its given rest
  /// orientation.
  ///
  /// @param[in] C  #C by 3 list of edge vertex positions
  /// @param[in] E  #E by 2 list of directed edges
  /// @param[out] Q  #E list of quaternions
  ///
  template <typename DerivedC, typename DerivedE>
  IGL_INLINE void directed_edge_orientations(
    const Eigen::MatrixBase<DerivedC> & C,
    const Eigen::MatrixBase<DerivedE> & E,
    std::vector<
      Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & Q);
}

#ifndef IGL_STATIC_LIBRARY
#  include "directed_edge_orientations.cpp"
#endif
#endif
