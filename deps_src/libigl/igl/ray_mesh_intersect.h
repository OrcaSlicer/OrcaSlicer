// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_RAY_MESH_INTERSECT_H
#define IGL_RAY_MESH_INTERSECT_H
#include "igl_inline.h"
#include "Hit.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  /// Shoot a ray against a mesh (V,F) and collect all hits. If you have many
  /// rays, consider using AABB.h
  ///
  /// @param[in] source  3-vector origin of ray
  /// @param[in] dir  3-vector direction of ray
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh face indices into V
  /// @param[out] hits  **sorted** list of hits
  /// @return true if there were any hits (hits.size() > 0)
  ///
  /// \see AABB
  template <
    typename Derivedsource,
    typename Deriveddir,
    typename DerivedV, 
    typename DerivedF> 
  IGL_INLINE bool ray_mesh_intersect(
    const Eigen::MatrixBase<Derivedsource> & source,
    const Eigen::MatrixBase<Deriveddir> & dir,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    std::vector<igl::Hit<typename DerivedV::Scalar>> & hits);
  /// \overload
  /// @param[in] hit  first hit, set only if it exists
  template <
    typename Derivedsource,
    typename Deriveddir,
    typename DerivedV, 
    typename DerivedF> 
  IGL_INLINE bool ray_mesh_intersect(
    const Eigen::MatrixBase<Derivedsource> & source,
    const Eigen::MatrixBase<Deriveddir> & dir,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    igl::Hit<typename DerivedV::Scalar> & hit);

  // Explicit function to check one triangle (given by index f) of the mesh.
  // This function is used by ray_mesh_intersect to check each triangle.
  // Outputs:
  //   hit  hit with the given triangle, set only if it exists
  // Returns true if there was a hit
  template <
    typename Derivedsource,
    typename Deriveddir,
    typename DerivedV,
    typename DerivedF>
  IGL_INLINE bool ray_triangle_intersect(
    const Eigen::MatrixBase<Derivedsource> & source,
    const Eigen::MatrixBase<Deriveddir> & dir,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const int f,
    igl::Hit<typename DerivedV::Scalar>& hit);
}
#ifndef IGL_STATIC_LIBRARY
#  include "ray_mesh_intersect.cpp"
#endif
#endif
