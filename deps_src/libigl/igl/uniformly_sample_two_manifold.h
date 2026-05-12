// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UNIFORMLY_SAMPLE_TWO_MANIFOLD_H
#define IGL_UNIFORMLY_SAMPLE_TWO_MANIFOLD_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  /// Attempt to sample a mesh uniformly with k-points by furthest point
  /// relaxation as described in "Fast Automatic Skinning Transformations"
  /// [Jacobson et al. 12] Section 3.3. The input is not expected to be a typical
  /// 3D triangle mesh (e.g., [V,F]), instead each vertex is embedded in a high
  /// dimensional unit-hypercude ("weight space") defined by W, with triangles
  /// given by F. This algorithm will first conduct furthest point sampling from
  /// the set of vertices and then attempt to relax the sampled points along the
  /// surface of the high-dimensional triangle mesh (i.e., the output points may
  /// be in the middle of triangles, not just at vertices). An additional "push"
  /// factor will repel samples away from the corners of the hypercube.
  ///
  /// @param[in] W  #W by dim positions of mesh in weight space
  /// @param[in] F  #F by 3 indices of triangles
  /// @param[in] k  number of samples
  /// @param[in] push  factor by which corners should be pushed away
  /// @param[out] WS  k by dim locations in weight space
  ///
  /// \see random_points_on_mesh
  ///
  IGL_INLINE void uniformly_sample_two_manifold(
    const Eigen::MatrixXd & W,
    const Eigen::MatrixXi & F, 
    const int k, 
    const double push,
    Eigen::MatrixXd & WS);
  /// \overload
  ///
  /// \fileinfo
  IGL_INLINE void uniformly_sample_two_manifold_at_vertices(
    const Eigen::MatrixXd & W,
    const int k, 
    const double push,
    Eigen::VectorXi & S);
}
#ifndef IGL_STATIC_LIBRARY
#  include "uniformly_sample_two_manifold.cpp"
#endif
#endif
